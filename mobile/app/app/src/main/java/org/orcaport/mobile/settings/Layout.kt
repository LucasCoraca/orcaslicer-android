package org.orcaport.mobile.settings

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

/** One option shown in a row: key, the vector element it edits (if per extruder) and per-row definition overrides. */
class LayoutOption(
    val key: String,
    /** "extruder_idx": the element of the selected extruder; a number: that element; null: whole option / element 0 */
    val idx: String?,
    val overrides: JSONObject,
    /** synthetic option (ConfigOptionDef built in Tab.cpp, e.g. extruders_count) */
    val customDef: JSONObject?,
)

sealed class LayoutLine {
    /** one option with its own label */
    class Single(val option: LayoutOption, val wiki: String?, val override: Boolean, val allIndices: Boolean) : LayoutLine()
    /** several options on one labelled row (Tab.cpp "Line") */
    class Multi(val label: String, val tooltip: String?, val tooltipOf: String?, val options: List<LayoutOption>, val wiki: String?) : LayoutLine()
    /** a desktop custom widget (bed shape dialog, compatible printers list, IMEX modes ...) */
    class Custom(val key: String, val label: String?, val wiki: String?) : LayoutLine()
    object Separator : LayoutLine()
}

class LayoutGroup(val title: String, val icon: String, val lines: List<LayoutLine>, val hideLabels: Boolean, val noLabelColumn: Boolean)
class LayoutPage(val title: String, val icon: String, val groups: List<LayoutGroup>, val marlinOnly: Boolean)

/** The settings pages generated from the desktop Tab.cpp (mobile/tools/gen_settings_layout.py). */
class Layout(val tabs: Map<String, List<LayoutPage>>, val customDefs: JSONObject) {
    companion object {
        fun load(ctx: Context, lists: JSONObject): Layout {
            val j = JSONObject(ctx.assets.open("settings_layout.json").bufferedReader().use { it.readText() })
            val tabs = mutableMapOf<String, List<LayoutPage>>()
            for (t in listOf("print", "filament", "printer"))
                tabs[t] = parsePages(j.getJSONArray(t), lists)
            return Layout(tabs, j.optJSONObject("custom_defs") ?: JSONObject())
        }

        private fun parsePages(a: JSONArray, lists: JSONObject) = (0 until a.length()).map { i ->
            val p = a.getJSONObject(i)
            val groups = p.getJSONArray("groups")
            LayoutPage(p.optString("title"), p.optString("icon"), (0 until groups.length()).map { gi ->
                val g = groups.getJSONObject(gi)
                LayoutGroup(g.optString("title"), g.optString("icon"), parseLines(g.getJSONArray("lines"), lists),
                    g.optBoolean("hide_labels"), g.optBoolean("no_label_column"))
            }, p.optBoolean("marlin_only"))
        }

        private fun option(o: JSONObject, keyField: String): LayoutOption {
            val ov = JSONObject()
            for (k in listOf("full_width", "is_code", "multiline", "readonly", "height", "width", "label", "tooltip", "sidetext", "gui_type"))
                if (o.has(k)) ov.put(k, o.get(k))
            return LayoutOption(o.getString(keyField), o.optString("idx").takeIf { it.isNotEmpty() }, ov, o.optJSONObject("custom_def"))
        }

        private fun parseLines(a: JSONArray, lists: JSONObject): List<LayoutLine> {
            val out = mutableListOf<LayoutLine>()
            for (i in 0 until a.length()) {
                val l = a.getJSONObject(i)
                when {
                    l.optBoolean("separator") -> out += LayoutLine.Separator
                    l.has("expand") -> {   // a list libslic3r computes (publishable_printer_retraction_options ...)
                        val keys = lists.optJSONArray(l.getString("expand")) ?: JSONArray()
                        val tpl = l.getJSONArray("template")
                        for (k in 0 until keys.length()) for (t in 0 until tpl.length()) {
                            val line = JSONObject(tpl.getJSONObject(t).toString())
                            line.put("option", keys.getString(k))
                            out += parseLines(JSONArray().put(line), lists)
                        }
                    }
                    l.has("custom") -> out += LayoutLine.Custom(l.getString("custom"), l.optString("label").takeIf { it.isNotEmpty() }, l.optString("wiki").takeIf { it.isNotEmpty() })
                    l.has("options") -> {
                        val opts = l.getJSONArray("options")
                        out += LayoutLine.Multi(l.optString("label"), l.optString("tooltip").takeIf { it.isNotEmpty() },
                            l.optString("tooltip_of").takeIf { it.isNotEmpty() },
                            (0 until opts.length()).map { option(opts.getJSONObject(it), "key") }, l.optString("wiki").takeIf { it.isNotEmpty() })
                    }
                    l.has("option") -> out += LayoutLine.Single(option(l, "option"), l.optString("wiki").takeIf { it.isNotEmpty() },
                        l.optBoolean("override"), l.optBoolean("all_indices"))
                }
            }
            return out
        }
    }
}
