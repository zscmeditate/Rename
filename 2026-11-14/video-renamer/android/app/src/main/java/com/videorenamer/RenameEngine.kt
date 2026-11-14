package com.videorenamer

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.util.Locale

/**
 * 重命名核心逻辑，与桌面版保持一致：
 *  - 只处理当前文件夹中的 .mp4 / .mkv（不递归，扩展名忽略大小写）
 *  - 排序键为不含扩展名的文件名，采用与资源管理器一致的数值排序
 *  - 编号：文件数 ≤ 99 用两位（01…99），> 99 统一三位（001…）
 *  - 两阶段改名：先全部改为唯一临时名，再改为目标名，避免互相覆盖
 */
object RenameEngine {

    /** 撤销记录：当前（编号）文件名 → 原文件名 */
    data class UndoEntry(val current: String, val original: String)

    data class RenameResult(
        val total: Int,
        val width: Int,
        val done: List<UndoEntry>,
        val errors: List<String>
    )

    data class UndoResult(
        val restored: Int,
        val done: List<UndoEntry>,
        val errors: List<String>,
        val skipped: List<String>
    )

    private data class Child(val docId: String, val name: String, val isDir: Boolean)

    private data class StageItem(
        val original: String,
        val ext: String,
        val tempName: String,
        val docId: String
    )

    private data class UndoStage(
        val current: String,
        val original: String,
        val tempName: String,
        val docId: String
    )

    // ------------------------------------------------------------------
    // 文件夹描述
    // ------------------------------------------------------------------

    fun folderName(tree: Uri): String {
        val docId = runCatching { DocumentsContract.getTreeDocumentId(tree) }
            .getOrNull().orEmpty()
        val rel = docId.substringAfter(':', docId).trimEnd('/')
        return rel.substringAfterLast('/').ifEmpty { "根目录" }
    }

    fun folderPath(tree: Uri): String {
        val docId = runCatching { DocumentsContract.getTreeDocumentId(tree) }
            .getOrNull().orEmpty()
        val colon = docId.indexOf(':')
        if (colon < 0) return docId
        val volume = docId.substring(0, colon)
        val rel = docId.substring(colon + 1)
        val root = if (volume.equals("primary", true)) "内部存储" else "存储卡($volume)"
        return if (rel.isEmpty()) root else "$root/$rel"
    }

    // ------------------------------------------------------------------
    // 重命名
    // ------------------------------------------------------------------

    fun renameAll(context: Context, tree: Uri): RenameResult {
        val children = listChildren(context, tree)
        val videos = children.filter { !it.isDir && isVideo(it.name) }
        if (videos.isEmpty()) return RenameResult(0, 2, emptyList(), emptyList())

        val items = videos.sortedWith { a, b -> compareNames(a.name, b.name) }
        val width = if (items.size > 99) 3 else 2
        val names = children.mapTo(HashSet()) { it.name }
        val errors = ArrayList<String>()
        val staged = ArrayList<StageItem>()
        var seq = 0

        // 第一阶段：全部改成唯一临时名
        for (item in items) {
            val ext = extensionOf(item.name)
            val temp = newTempName(names, seq++)
            try {
                val newId = rename(context, tree, item.docId, temp)
                names.remove(item.name)
                names.add(temp)
                staged.add(StageItem(item.name, ext, temp, newId))
            } catch (e: Exception) {
                errors.add("【${item.name}】改名失败（第一步）：${reasonOf(e)}")
            }
        }

        // 第二阶段：临时名改为编号 + 原扩展名
        val done = ArrayList<UndoEntry>()
        staged.forEachIndexed { index, s ->
            val target = String.format(Locale.ROOT, "%0${width}d", index + 1) + s.ext
            try {
                rename(context, tree, s.docId, target)
                done.add(UndoEntry(target, s.original))
            } catch (e: Exception) {
                val why = reasonOf(e)
                try {
                    rename(context, tree, s.docId, s.original)
                    errors.add("【${s.original}】改名失败（第二步）：$why（已恢复原文件名）")
                } catch (e2: Exception) {
                    errors.add(
                        "【${s.original}】改名失败（第二步）：$why" +
                            "；且恢复原文件名失败，当前名为：${s.tempName}"
                    )
                }
            }
        }

        return RenameResult(items.size, width, done, errors)
    }

    // ------------------------------------------------------------------
    // 撤销
    // ------------------------------------------------------------------

    fun undo(context: Context, tree: Uri, entries: List<UndoEntry>): UndoResult {
        val children = listChildren(context, tree)
        val byName = children.associateBy { it.name }
        val names = children.mapTo(HashSet()) { it.name }
        val errors = ArrayList<String>()
        val skipped = ArrayList<String>()
        val staged = ArrayList<UndoStage>()
        var seq = 0

        for (entry in entries) {
            val child = byName[entry.current]
            if (child == null) {
                skipped.add(entry.current)
                continue
            }
            val temp = newTempName(names, seq++)
            try {
                val newId = rename(context, tree, child.docId, temp)
                names.remove(entry.current)
                names.add(temp)
                staged.add(UndoStage(entry.current, entry.original, temp, newId))
            } catch (e: Exception) {
                errors.add("【${entry.current}】撤销失败（第一步）：${reasonOf(e)}")
            }
        }

        val done = ArrayList<UndoEntry>()
        var restored = 0
        for (s in staged) {
            try {
                rename(context, tree, s.docId, s.original)
                restored++
                done.add(UndoEntry(s.current, s.original))
            } catch (e: Exception) {
                val why = reasonOf(e)
                try {
                    rename(context, tree, s.docId, s.current)
                    errors.add("【${s.current}】撤销失败（第二步）：$why（已保留编号文件名）")
                } catch (e2: Exception) {
                    errors.add(
                        "【${s.current}】撤销失败（第二步）：$why" +
                            "；且恢复编号文件名失败，当前名为：${s.tempName}"
                    )
                }
            }
        }

        return UndoResult(restored, done, errors, skipped)
    }

    // ------------------------------------------------------------------
    // 文件枚举与工具方法
    // ------------------------------------------------------------------

    private fun listChildren(context: Context, tree: Uri): List<Child> {
        val treeDocId = DocumentsContract.getTreeDocumentId(tree)
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, treeDocId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE
        )
        val out = ArrayList<Child>()
        context.contentResolver.query(childrenUri, projection, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getString(0) ?: continue
                val name = c.getString(1) ?: continue
                val isDir = DocumentsContract.Document.MIME_TYPE_DIR == c.getString(2)
                out.add(Child(id, name, isDir))
            }
        } ?: throw IllegalStateException("无法读取文件夹内容")
        return out
    }

    private fun rename(context: Context, tree: Uri, docId: String, newName: String): String {
        val docUri = DocumentsContract.buildDocumentUriUsingTree(tree, docId)
        val renamed = DocumentsContract.renameDocument(context.contentResolver, docUri, newName)
            ?: throw IllegalStateException("系统未返回重命名结果")
        return DocumentsContract.getDocumentId(renamed)
    }

    private fun newTempName(names: Set<String>, seq: Int): String {
        val seed = System.currentTimeMillis()
        var k = 0
        while (true) {
            val candidate = ".vrn_tmp_${seed}_${seq}_$k.tmp"
            if (candidate !in names) return candidate
            k++
        }
    }

    private fun reasonOf(e: Exception): String =
        e.message?.takeIf { it.isNotBlank() } ?: e.javaClass.simpleName

    private fun isVideo(name: String): Boolean {
        val lower = name.lowercase(Locale.ROOT)
        return lower.endsWith(".mp4") || lower.endsWith(".mkv")
    }

    private fun extensionOf(name: String): String {
        val dot = name.lastIndexOf('.')
        return if (dot > 0) name.substring(dot) else ""
    }

    private fun baseName(name: String): String {
        val ext = extensionOf(name)
        return if (ext.isEmpty()) name else name.substring(0, name.length - ext.length)
    }

    /** 主名优先（不比较后缀），主名相同时再比较扩展名，保证顺序稳定 */
    fun compareNames(a: String, b: String): Int {
        val c = naturalCompare(baseName(a), baseName(b))
        if (c != 0) return c
        return naturalCompare(extensionOf(a), extensionOf(b))
    }

    /** 与资源管理器一致的数值排序：数字段按数值比较，文本忽略大小写 */
    fun naturalCompare(a: String, b: String): Int {
        var i = 0
        var j = 0
        while (i < a.length && j < b.length) {
            val ca = a[i]
            val cb = b[j]
            if (ca.isDigit() && cb.isDigit()) {
                val si = i
                val sj = j
                while (i < a.length && a[i].isDigit()) i++
                while (j < b.length && b[j].isDigit()) j++
                val na = a.substring(si, i)
                val nb = b.substring(sj, j)
                val ta = na.trimStart('0')
                val tb = nb.trimStart('0')
                if (ta.length != tb.length) return ta.length - tb.length
                val c = ta.compareTo(tb)
                if (c != 0) return c
                if (na.length != nb.length) return na.length - nb.length
            } else {
                val la = ca.lowercaseChar()
                val lb = cb.lowercaseChar()
                if (la != lb) return la.compareTo(lb)
                if (ca != cb) return ca.compareTo(cb)
                i++
                j++
            }
        }
        return (a.length - i) - (b.length - j)
    }
}
