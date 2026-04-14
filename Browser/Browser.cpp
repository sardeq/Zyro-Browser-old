#include "Browser.h"
#include "Globals.h"
#include "Utils.h"
#include "Storage.h"
#include "Downloads.h"
#include "Blocker.h"

#include <iostream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cstring> 
#include <fstream>
#include <iomanip>

#ifdef __linux__
#include <malloc.h>
#endif

static std::vector<std::string> closed_tabs_history;
static std::vector<std::string> last_session_urls; 

void update_media_popup();
void create_media_player_popover(GtkWidget* btn);
void send_media_command(WebKitWebView* view, const std::string& command);

GtkWidget* create_menu_icon(const char* icon_name) {
    GtkWidget* img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
    return img;
}


void save_session_to_disk(GtkWidget* win) {
    GtkNotebook* nb = get_notebook(win);
    if (!nb) return;

    std::ofstream f(get_user_data_dir() + "last_session.txt");
    int n_pages = gtk_notebook_get_n_pages(nb);
    
    for (int i = 0; i < n_pages; i++) {
        GtkWidget* page = gtk_notebook_get_nth_page(nb, i);
        GList* children = gtk_container_get_children(GTK_CONTAINER(page));
        for (GList* l = children; l; l = l->next) {
            if (WEBKIT_IS_WEB_VIEW(l->data)) {
                WebKitWebView* view = WEBKIT_WEB_VIEW(l->data);
                
                webkit_web_view_stop_loading(view);

                const char* uri = webkit_web_view_get_uri(view);
                if (uri && strlen(uri) > 0) {
                    f << uri << "\n";
                }
                break;
            }
        }
        g_list_free(children);
    }
}

void load_session_into_memory() {
    last_session_urls.clear();
    std::ifstream f(get_user_data_dir() + "last_session.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) last_session_urls.push_back(line);
    }
}


void show_site_info_popover(GtkEntry* entry, WebKitWebView* view);
void show_menu(GtkButton* btn, gpointer win);
void on_url_activate(GtkEntry* e, gpointer view_ptr);
static void on_url_changed(GtkEditable* editable, gpointer user_data);
static GtkWidget* on_web_view_create(WebKitWebView* web_view, WebKitNavigationAction* navigation_action, gpointer user_data);
static void on_media_btn_clicked(GtkButton* btn, gpointer);

extern int get_blocked_count();

void run_js(WebKitWebView* view, const std::string& script) {
    if (WEBKIT_IS_WEB_VIEW(view)) {
        webkit_web_view_evaluate_javascript(view, script.c_str(), -1, NULL, NULL, NULL, NULL, NULL);
    }
}

GtkNotebook* get_notebook(GtkWidget* win) {
    return GTK_NOTEBOOK(g_object_get_data(G_OBJECT(win), "notebook"));
}

WebKitWebView* get_active_webview(GtkWidget* win) {
    GtkNotebook* nb = get_notebook(win);
    int page_num = gtk_notebook_get_current_page(nb);
    if (page_num == -1) return nullptr;
    
    GtkWidget* page = gtk_notebook_get_nth_page(nb, page_num);
    GList* children = gtk_container_get_children(GTK_CONTAINER(page));
    
    WebKitWebView* view = nullptr;
    for (GList* l = children; l != NULL; l = l->next) {
        if (WEBKIT_IS_WEB_VIEW(l->data)) {
            view = WEBKIT_WEB_VIEW(l->data);
            break;
        }
    }
    g_list_free(children);
    return view;
}

struct SuggestionRequest {
    bool is_gtk;
    gpointer target;
    std::string query;
};

std::vector<std::string> parse_remote_suggestions(const std::string& json) {
    std::vector<std::string> results;
    try {
        size_t start_arr = json.find('[', 1); 
        size_t end_arr = json.find(']', start_arr);
        if (start_arr == std::string::npos || end_arr == std::string::npos) return results;

        std::string list_content = json.substr(start_arr + 1, end_arr - start_arr - 1);
        std::regex re("\"([^\"]+)\"");
        auto begin = std::sregex_iterator(list_content.begin(), list_content.end(), re);
        auto end = std::sregex_iterator();

        for (auto i = begin; i != end; ++i) {
            results.push_back((*i)[1].str());
        }
    } catch (...) {}
    return results;
}

std::vector<std::string> get_combined_suggestions(const std::string& query, const std::string& remote_json, bool include_urls) {
    std::vector<std::string> combined;
    std::string q_lower = query;
    std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);

    int count = 0;
    for (auto it = search_history.rbegin(); it != search_history.rend(); ++it) {
        if (count > 2) break; 
        std::string s_lower = *it;
        std::transform(s_lower.begin(), s_lower.end(), s_lower.begin(), ::tolower);
        
        if (s_lower.find(q_lower) == 0) { 
            combined.push_back("[H] " + *it); 
            count++;
        }
    }

    if (include_urls) {
        int hist_links_count = 0;
        for (auto it = browsing_history.rbegin(); it != browsing_history.rend(); ++it) {
            if (hist_links_count > 50) break; 
            
            std::string u_lower = it->url;
            std::transform(u_lower.begin(), u_lower.end(), u_lower.begin(), ::tolower);
            
            if (u_lower.find(q_lower) != std::string::npos) {
                bool already_added = false;
                for(const auto& s : combined) if(s == it->url) already_added = true;
                
                if(!already_added) {
                    combined.push_back(it->url);
                    hist_links_count++;
                }
            }
        }
    }

    if (!remote_json.empty()) {
        std::vector<std::string> remote = parse_remote_suggestions(remote_json);
        for (const auto& r : remote) {
            bool exists = false;
            for(const auto& c : combined) {
                if(c.find(r) != std::string::npos) exists = true;
            }
            if(!exists) combined.push_back(r);
        }
    }
    return combined;
}

void on_suggestion_ready(GObject* source, GAsyncResult* res, gpointer user_data) {
    SoupSession* session = SOUP_SESSION(source);
    SuggestionRequest* req = (SuggestionRequest*)user_data;
    GError* error = nullptr;

    GBytes* body_bytes = soup_session_send_and_read_finish(session, res, &error);
    
    std::string body = "";
    if (body_bytes) {
        gsize size;
        const char* data = (const char*)g_bytes_get_data(body_bytes, &size);
        body.assign(data, size);
        g_bytes_unref(body_bytes);
    }
    
    std::vector<std::string> final_list = get_combined_suggestions(req->query, body, req->is_gtk);

    if (req->is_gtk) {
        GtkListStore* store = nullptr;
        if (GTK_IS_ENTRY_COMPLETION(req->target)) {
            store = GTK_LIST_STORE(gtk_entry_completion_get_model(GTK_ENTRY_COMPLETION(req->target)));
        } else if (GTK_IS_LIST_STORE(req->target)) {
            store = GTK_LIST_STORE(req->target);
        }

        if (store) {
            gtk_list_store_clear(store);
            GtkTreeIter iter;
            
            int limit = 20;
            for (const auto& s : final_list) {
                if(limit-- <= 0) break;
                std::string text = s;
                
                if(text.find("[H] ") == 0) text = text.substr(4);

                if (text.find("https://") == 0) text = text.substr(8);
                else if (text.find("http://") == 0) text = text.substr(7);
                
                if (text.find("www.") == 0) text = text.substr(4);
                
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, text.c_str(), -1);
            }
        }
        
        // Only trigger the "popup" if it is an actual completion widget
        // bit buggy
        if(GTK_IS_ENTRY_COMPLETION(req->target) && final_list.size() > 0) {
            gtk_entry_completion_complete(GTK_ENTRY_COMPLETION(req->target));
        }
    } else {
        std::stringstream js_array;
        js_array << "[";
        for (size_t i = 0; i < final_list.size(); ++i) {
            js_array << "'" << final_list[i] << "'";
            if (i < final_list.size() - 1) js_array << ",";
        }
        js_array << "]";

        std::string script = "renderSuggestions(" + js_array.str() + ");";
        run_js(WEBKIT_WEB_VIEW(req->target), script);
    }
    delete req;
}

void fetch_suggestions(const std::string& query, bool is_gtk, gpointer target) {
    if (query.length() < 1) return; 
    
    SuggestionRequest* req = new SuggestionRequest{ is_gtk, target, query };
    std::string url = settings.suggestion_api + query;
    
    SoupMessage* msg = soup_message_new("GET", url.c_str());
    
    soup_session_send_and_read_async(soup_session, msg, G_PRIORITY_DEFAULT, NULL, on_suggestion_ready, req);
    g_object_unref(msg);
}

static void on_script_message(WebKitUserContentManager* manager, WebKitJavascriptResult* res, gpointer user_data) {
    JSCValue* value = webkit_javascript_result_get_js_value(res);
    char* json_str = jsc_value_to_string(value);
    std::string json(json_str);
    g_free(json_str);

    auto get_json_val = [&](std::string key) -> std::string {
        std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(json, match, re)) return match[1].str();
        return "";
    };
    
    auto get_json_bool = [&](std::string key) -> bool {
         std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
         std::smatch match;
         if (std::regex_search(json, match, re)) {
             return match[1].str() == "true";
         }
         return false;
    };

    auto get_json_int = [&](std::string key) -> int {
        std::regex re("\"" + key + "\"\\s*:\\s*([0-9]+)");
        std::smatch match;
        if (std::regex_search(json, match, re)) return std::stoi(match[1].str());
        return -1;
    };

    std::string type = get_json_val("type");
    WebKitWebView* view = WEBKIT_WEB_VIEW(user_data);
    WebKitWebContext* ctx = webkit_web_view_get_context(view);
    bool is_incognito = webkit_web_context_is_ephemeral(ctx);

    if (type == "search") {
        std::string query = get_json_val("query");
        add_search_query(query); 
        std::string url = settings.search_engine + query;
        webkit_web_view_load_uri(view, url.c_str());
    }
    else if (type == "get_suggestions") {
        fetch_suggestions(get_json_val("query"), false, view);
    }
    else if (type == "save_settings") {
        std::string engine = get_json_val("engine");
        std::string theme = get_json_val("theme");
        if (engine.empty()) engine = settings.search_engine;
        if (theme.empty()) theme = settings.theme;
        
        save_settings(engine, theme);
        apply_browser_theme(theme); // Update GTK
        
        // Notify all tabs about theme change (FIXED LOOP)
        if(global_window) {
             GtkNotebook* nb = get_notebook(global_window);
             int pages = gtk_notebook_get_n_pages(nb);
             for(int i=0; i<pages; i++) {
                 GtkWidget* page_box = gtk_notebook_get_nth_page(nb, i);
                 GList* children = gtk_container_get_children(GTK_CONTAINER(page_box));
                 for(GList* l = children; l; l = l->next) {
                     if(WEBKIT_IS_WEB_VIEW(l->data)) {
                        run_js(WEBKIT_WEB_VIEW(l->data), "setTheme('" + theme + "');");
                        break;
                     }
                 }
                 g_list_free(children);
             }
        }
    }
    else if (type == "save_home_prefs") {
        // Save Home Page Specific Customizations
        settings.bg_type = get_json_val("bg_type");
        settings.show_cpu = get_json_bool("show_cpu");
        settings.show_ram = get_json_bool("show_ram");
        settings.show_shortcuts = get_json_bool("show_shortcuts");
        
        std::string theme = get_json_val("theme");
        if (!theme.empty()) {
            settings.theme = theme;
            apply_browser_theme(theme);
        }

        save_settings(settings.search_engine, settings.theme);
        
        // Refresh tabs with new theme (FIXED LOOP)
        if(global_window) {
             GtkNotebook* nb = get_notebook(global_window);
             int pages = gtk_notebook_get_n_pages(nb);
             for(int i=0; i<pages; i++) {
                 GtkWidget* page_box = gtk_notebook_get_nth_page(nb, i);
                 GList* children = gtk_container_get_children(GTK_CONTAINER(page_box));
                 for(GList* l = children; l; l = l->next) {
                     if(WEBKIT_IS_WEB_VIEW(l->data)) {
                        WebKitWebView* v = WEBKIT_WEB_VIEW(l->data);
                        // Update theme
                        run_js(v, "setTheme('" + settings.theme + "');");
                        
                        // Update Home prefs if applicable
                        const char* uri = webkit_web_view_get_uri(v);
                        if (uri && std::string(uri).find("home.html") != std::string::npos) {
                            std::string bool_cpu = settings.show_cpu ? "true" : "false";
                            std::string bool_ram = settings.show_ram ? "true" : "false";
                            std::string bool_sc = settings.show_shortcuts ? "true" : "false";
                            
                            std::string script = "applyPreferences(" + bool_cpu + ", " + bool_ram + ", " + bool_sc + ", '" + settings.bg_type + "');";
                            run_js(v, script);
                        }
                        break; 
                     }
                 }
                 g_list_free(children);
             }
        }
    }
    else if (type == "get_home_prefs") {
        std::string bool_cpu = settings.show_cpu ? "true" : "false";
        std::string bool_ram = settings.show_ram ? "true" : "false";
        std::string bool_sc = settings.show_shortcuts ? "true" : "false";
        
        std::stringstream ss;
        ss << "{";
        ss << "\"show_cpu\": " << bool_cpu << ",";
        ss << "\"show_ram\": " << bool_ram << ",";
        ss << "\"show_shortcuts\": " << bool_sc << ",";
        ss << "\"bg_type\": \"" << settings.bg_type << "\",";
        ss << "\"theme\": \"" << settings.theme << "\"";
        ss << "}";
        
        run_js(view, "loadPreferences(" + ss.str() + ");");
    }
    else if (type == "get_bookmarks") {
        if (is_incognito) {
            run_js(view, "renderBookmarks('[]');");
        } else {
            std::stringstream ss; ss << "[";
            for(size_t i=0; i<bookmarks_list.size(); ++i) {
                ss << "{ \"title\": \"" << bookmarks_list[i].title << "\", \"url\": \"" << bookmarks_list[i].url << "\" }";
                if(i < bookmarks_list.size()-1) ss << ",";
            }
            ss << "]";
            run_js(view, "renderBookmarks('" + ss.str() + "');");
        }
    }
    else if (type == "delete_bookmark") {
        int idx = get_json_int("index");
        if(idx >= 0 && idx < bookmarks_list.size()) {
            bookmarks_list.erase(bookmarks_list.begin() + idx);
            save_bookmarks_to_disk();
        }
    }
    else if (type == "rename_bookmark") {
        int idx = get_json_int("index");
        std::string new_title = get_json_val("new_title");
        if(idx >= 0 && idx < bookmarks_list.size() && !new_title.empty()) {
            bookmarks_list[idx].title = new_title;
            save_bookmarks_to_disk();
        }
    }
    else if (type == "get_shortcuts") {
        if (is_incognito) {
             run_js(view, "renderShortcuts([]);");
        } else {
            std::stringstream ss; ss << "[";
            for(size_t i=0; i<shortcuts_list.size(); ++i) {
                ss << "{ \"name\": \"" << shortcuts_list[i].name << "\", \"url\": \"" << shortcuts_list[i].url << "\" }";
                if(i < shortcuts_list.size()-1) ss << ",";
            }
            ss << "]";
            run_js(view, "renderShortcuts(" + ss.str() + ");");
        }
    }
    else if (type == "add_shortcut") {
        shortcuts_list.push_back({ get_json_val("name"), get_json_val("url") });
        save_shortcuts_to_disk();
    }
    else if (type == "delete_shortcut") {
        int idx = get_json_int("index");
        if(idx >= 0 && idx < (int)shortcuts_list.size()) {
            shortcuts_list.erase(shortcuts_list.begin() + idx);
            save_shortcuts_to_disk();
        }
    }
    else if (type == "edit_shortcut") {
        int idx = get_json_int("index");
        if(idx >= 0 && idx < (int)shortcuts_list.size()) {
            shortcuts_list[idx].name = get_json_val("name");
            shortcuts_list[idx].url = get_json_val("url");
            save_shortcuts_to_disk();
        }
    }
    else if (type == "get_theme") {
        run_js(view, "setTheme('" + settings.theme + "');");
    }
    else if (type == "get_downloads") {
        std::stringstream ss; ss << "[";
        for(size_t i=0; i<downloads_list.size(); ++i) {
            ss << "{ \"id\": " << downloads_list[i].id << ", "
               << "\"filename\": \"" << downloads_list[i].filename << "\", "
               << "\"url\": \"" << downloads_list[i].url << "\", "
               << "\"status\": \"" << downloads_list[i].status << "\", "
               << "\"progress\": " << downloads_list[i].progress << " }";
            if(i < downloads_list.size()-1) ss << ",";
        }
        ss << "]";
        run_js(view, "renderDownloads('" + ss.str() + "');");
    }
    else if (type == "stop_download") {
        int id = get_json_int("id");
        int idx = find_download_index((guint64)id);
        if (idx != -1 && downloads_list[idx].status == "Downloading") {
            webkit_download_cancel(downloads_list[idx].webkit_download);
        }
    }
    else if (type == "clear_downloads") {
        auto it = std::remove_if(downloads_list.begin(), downloads_list.end(), 
            [](const DownloadItem& i){ return i.status != "Downloading"; });
        downloads_list.erase(it, downloads_list.end());
    }
    else if (type == "get_passwords") {
        std::stringstream ss; ss << "[";
        for(size_t i=0; i<saved_passwords.size(); ++i) {
            ss << "{ \"site\": \"" << saved_passwords[i].site << "\", "
               << "\"user\": \"" << saved_passwords[i].user << "\", "
               << "\"pass\": \"" << saved_passwords[i].pass << "\" }";
            if(i < saved_passwords.size()-1) ss << ",";
        }
        ss << "]";
        run_js(view, "renderPasswords('" + ss.str() + "');");
    }
    else if (type == "save_password") {
        saved_passwords.push_back({get_json_val("site"), get_json_val("user"), get_json_val("pass")});
        save_passwords_to_disk();
    }
    else if (type == "delete_password") {
        int idx = get_json_int("index");
        if(idx >= 0 && idx < saved_passwords.size()) {
            saved_passwords.erase(saved_passwords.begin() + idx);
            save_passwords_to_disk();
        }
    }
    else if (type == "get_settings") {
        std::string script = "loadCurrentSettings('" + settings.search_engine + "', '" + settings.theme + "');";
        run_js(view, script);
    }
    else if (type == "clear_cache") {
        WebKitWebsiteDataManager* mgr = webkit_web_context_get_website_data_manager(global_context);
        webkit_website_data_manager_clear(mgr, WEBKIT_WEBSITE_DATA_ALL, 0, NULL, NULL, NULL);
        // Force drop memory caches
        webkit_web_context_clear_cache(global_context);
        run_js(view, "showToast('Cache Cleared');"); 
    }
    else if (type == "open_url") {
        webkit_web_view_load_uri(view, get_json_val("url").c_str());
    }
    else if (type == "get_history") {
        if (is_incognito) {
            run_js(view, "renderHistory('[]');");
        } else {
            std::stringstream ss;
            ss << "[";
            for(size_t i=0; i<browsing_history.size(); ++i) {
                const auto& h = browsing_history[i];
                ss << "{ \"title\": \"" << json_escape(h.title) << "\", "
                << "\"url\": \"" << json_escape(h.url) << "\", "
                << "\"time\": \"" << h.time_str << "\" }";
                if(i < browsing_history.size()-1) ss << ",";
            }
            ss << "]";
            
            std::string json_data = ss.str();
            std::string safe_json; 
            for(char c : json_data) {
                if(c == '\'') safe_json += "\\'";
                else safe_json += c;
            }

            std::string script = "renderHistory('" + safe_json + "');";
            run_js(view, script);
        }
        }
        else if (type == "clear_history") {
            clear_all_history();
            run_js(view, "renderHistory('[]');");
        }
        else if (type == "get_tasks") {
        GtkNotebook* nb = get_notebook(global_window);
        int n_pages = gtk_notebook_get_n_pages(nb);

        std::stringstream ss; 
        ss << "[";

        for (int i = 0; i < n_pages; i++) {
            GtkWidget* page = gtk_notebook_get_nth_page(nb, i);
            GList* children = gtk_container_get_children(GTK_CONTAINER(page));
            WebKitWebView* tab_view = nullptr;
            
            for(GList* l = children; l; l = l->next) {
                if(WEBKIT_IS_WEB_VIEW(l->data)) { 
                    tab_view = WEBKIT_WEB_VIEW(l->data); 
                    break; 
                }
            }
            g_list_free(children);

            if(tab_view) {
                const char* title = webkit_web_view_get_title(tab_view);
                const char* url = webkit_web_view_get_uri(tab_view);
                
                int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tab_view), "web_pid"));
                
                std::string mem = "-";
                
                if (pid > 0) {
                    long kb = get_pid_rss_kb(pid);
                    std::stringstream mss;
                    mss << std::fixed << std::setprecision(1) << (kb / 1024.0) << " MB";
                    mem = mss.str();
                }

                ss << "{ \"id\": " << i << ", "
                   << "\"title\": \"" << json_escape(title ? title : "Loading...") << "\", "
                   << "\"url\": \"" << json_escape(url ? url : "") << "\", "
                   << "\"pid\": " << pid << ", "
                   << "\"mem\": \"" << mem << "\" }";
                
                if(i < n_pages - 1) ss << ",";
            }
        }
        ss << "]";
        
        std::string total_mem = get_total_memory_str();
        std::string script = "renderTasks(" + ss.str() + ", '" + total_mem + "');";
        run_js(view, script);
    }
    else if (type == "close_task_tab") {
        int idx = get_json_int("index");
        GtkNotebook* nb = get_notebook(global_window);
        
        if(idx >= 0 && idx < gtk_notebook_get_n_pages(nb)) {

            gtk_notebook_remove_page(nb, idx);
            
            run_js(view, "refreshTasks();");
        }
    }
    else if (type == "get_incognito_status") {
        std::string status = is_incognito ? "true" : "false";
        run_js(view, "setIncognitoMode(" + status + ");");
    }
    else if (type == "save_performance_settings") {
        settings.hardware_acceleration = get_json_bool("hw_accel");
        settings.enable_memory_trim = get_json_bool("mem_trim");
        settings.cache_model = get_json_val("cache_model");
        
        save_settings(settings.search_engine, settings.theme); 
        run_js(view, "showToast('Performance settings will apply after restart');");
    }
    else if (type == "get_performance_settings") {
        std::stringstream ss;
        ss << "{";
        ss << "\"hw_accel\": " << (settings.hardware_acceleration ? "true" : "false") << ",";
        ss << "\"mem_trim\": " << (settings.enable_memory_trim ? "true" : "false") << ",";
        ss << "\"cache_model\": \"" << settings.cache_model << "\"";
        ss << "}";
        
        run_js(view, "loadPerformanceSettings(" + ss.str() + ");");
    }
}

void try_autofill(WebKitWebView* view, const char* uri) {
    if (!global_security.ready) return;
    std::string current_url(uri);

    for (const auto& item : saved_passwords) {
        if (current_url.find(item.site) != std::string::npos) {
            std::string script = 
                "var u = '" + item.user + "';"
                "var p = '" + item.pass + "';"
                "var passInputs = document.querySelectorAll('input[type=\"password\"]');"
                "if (passInputs.length > 0) {"
                "  passInputs[0].value = p;"
                "  passInputs[0].dispatchEvent(new Event('input', { bubbles: true }));"
                "  passInputs[0].dispatchEvent(new Event('change', { bubbles: true }));"
                "  var inputs = document.querySelectorAll('input[type=\"text\"], input[type=\"email\"]');"
                "  for (var i = 0; i < inputs.length; i++) {"
                "    if (inputs[i].compareDocumentPosition(passInputs[0]) & Node.DOCUMENT_POSITION_FOLLOWING) {"
                "       inputs[i].value = u;"
                "       inputs[i].dispatchEvent(new Event('input', { bubbles: true }));"
                "       inputs[i].dispatchEvent(new Event('change', { bubbles: true }));"
                "       break;"
                "    }"
                "  }"
                "}";
            
            run_js(view, script);
            return; 
        }
    }
}

void refresh_bookmarks_bar(GtkWidget* bar) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(bar));
    for(GList* iter = children; iter != NULL; iter = g_list_next(iter))
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    g_list_free(children);

    for (size_t i = 0; i < bookmarks_list.size(); ++i) {
        GtkWidget* btn = gtk_button_new_with_label(bookmarks_list[i].title.c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "bookmark-item");
        
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer url_ptr){
            std::string* u = (std::string*)url_ptr;
            WebKitWebView* v = get_active_webview(global_window);
            if(v) webkit_web_view_load_uri(v, u->c_str());
            delete u;
        }), new std::string(bookmarks_list[i].url));

        g_signal_connect(btn, "button-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventButton* event, gpointer idx_ptr) -> gboolean {
            if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
                int index = GPOINTER_TO_INT(idx_ptr);
                
                GtkWidget* menu = gtk_menu_new();
                GtkWidget* rename_item = gtk_menu_item_new_with_label("Rename");
                GtkWidget* del_item = gtk_menu_item_new_with_label("Delete Bookmark");
                
                g_signal_connect(rename_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data){
                    int idx = GPOINTER_TO_INT(data);
                    GtkWidget* dialog = gtk_dialog_new_with_buttons("Rename Bookmark", GTK_WINDOW(global_window), GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
                    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
                    GtkWidget* entry = gtk_entry_new();
                    gtk_entry_set_text(GTK_ENTRY(entry), bookmarks_list[idx].title.c_str());
                    gtk_container_add(GTK_CONTAINER(content), entry);
                    gtk_widget_show_all(dialog);

                    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
                        const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
                        if (text && strlen(text) > 0) {
                            bookmarks_list[idx].title = text;
                            save_bookmarks_to_disk();
                            GtkNotebook* nb = get_notebook(global_window);
                            for(int i=0; i<gtk_notebook_get_n_pages(nb); i++){
                                GtkWidget* pb = gtk_notebook_get_nth_page(nb, i);
                                GtkWidget* bb = (GtkWidget*)g_object_get_data(G_OBJECT(pb), "bookmarks_bar");
                                if(bb) refresh_bookmarks_bar(bb);
                            }
                        }
                    }
                    gtk_widget_destroy(dialog);
                }), GINT_TO_POINTER(index));

                g_signal_connect(del_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data){
                    int idx = GPOINTER_TO_INT(data);
                    bookmarks_list.erase(bookmarks_list.begin() + idx);
                    save_bookmarks_to_disk();
                    GtkNotebook* nb = get_notebook(global_window);
                    for(int i=0; i<gtk_notebook_get_n_pages(nb); i++){
                        GtkWidget* pb = gtk_notebook_get_nth_page(nb, i);
                        GtkWidget* bb = (GtkWidget*)g_object_get_data(G_OBJECT(pb), "bookmarks_bar");
                        if(bb) refresh_bookmarks_bar(bb);
                    }
                }), GINT_TO_POINTER(index));

                gtk_menu_shell_append(GTK_MENU_SHELL(menu), rename_item);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), del_item);
                gtk_widget_show_all(menu);
                gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
                return TRUE;
            }
            return FALSE;
        }), GINT_TO_POINTER(i));

        gtk_box_pack_start(GTK_BOX(bar), btn, FALSE, FALSE, 2);
    }
    gtk_widget_show_all(bar);
}

static gboolean on_user_message_received(WebKitWebView* view, WebKitUserMessage* message, gpointer user_data) {
    const char* name = webkit_user_message_get_name(message);
    
    if (strcmp(name, "pid-update") == 0) {
        GVariant* params = webkit_user_message_get_parameters(message);
        if (params) {
            const char* pid_str = g_variant_get_string(params, NULL);
            int pid = std::stoi(pid_str);
            
            g_object_set_data(G_OBJECT(view), "web_pid", GINT_TO_POINTER(pid));
            
            //std::cout << "Tab matched to PID: " << pid << std::endl; // debugging
        }
        return TRUE;
    }
    return FALSE;
}


static void on_url_changed(GtkEditable* editable, gpointer user_data) {
    std::string text = gtk_entry_get_text(GTK_ENTRY(editable));
    if (text.find("://") != std::string::npos) return;
    
    GtkEntryCompletion* completion = gtk_entry_get_completion(GTK_ENTRY(editable));
    fetch_suggestions(text, true, completion);
}

static gboolean on_match_selected(GtkEntryCompletion* widget, GtkTreeModel* model, GtkTreeIter* iter, gpointer entry_ptr) {
    gchar* value;
    gtk_tree_model_get(model, iter, 0, &value, -1);
    
    std::string url(value);
    g_free(value);

    GtkEntry* entry = GTK_ENTRY(entry_ptr);
    gtk_entry_set_text(entry, url.c_str());
    
    g_signal_emit_by_name(entry, "activate");
    return TRUE;
}

void on_url_activate(GtkEntry* e, gpointer view_ptr) {
    WebKitWebView* v = WEBKIT_WEB_VIEW(view_ptr);
    if (!v) return;
    std::string t = gtk_entry_get_text(e);
    
    bool is_url = false;
    
    if (t.find("://") != std::string::npos) is_url = true;
    else if (t.find(".") != std::string::npos && t.find(" ") == std::string::npos) is_url = true;
    else if (t.find("localhost") != std::string::npos) is_url = true;

    if (is_url) {
        if (t.find("://") == std::string::npos) {
            t = "https://" + t;
        }
        webkit_web_view_load_uri(v, t.c_str());
    } else {
        add_search_query(t);
        t = settings.search_engine + t;
        webkit_web_view_load_uri(v, t.c_str());
    }
    gtk_widget_grab_focus(GTK_WIDGET(v));
}

void update_url_bar(WebKitWebView* web_view, GtkEntry* entry) {
    const char* uri = webkit_web_view_get_uri(web_view);
    if (!uri) return;

    std::string u(uri);
    
    if(u.find("file://") == std::string::npos) {
        gtk_entry_set_text(entry, uri); 
        
        if(u.find("https://") == 0) {
            gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_PRIMARY, "channel-secure-symbolic");
            gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_PRIMARY, "View site information");
        } else {
            gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_PRIMARY, "channel-insecure-symbolic");
            gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_PRIMARY, "Connection is not secure");
        }
        
        gtk_entry_set_icon_activatable(entry, GTK_ENTRY_ICON_PRIMARY, TRUE);

        const char* title = webkit_web_view_get_title(web_view);
        add_history_item(u, title ? std::string(title) : "");

        auto it = std::find_if(bookmarks_list.begin(), bookmarks_list.end(), 
            [&](const BookmarkItem& b){ return b.url == u; });

        if (it != bookmarks_list.end()) {
            gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "starred-symbolic");
        } else {
            gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "non-starred-symbolic");
        }

    } else {
        gtk_entry_set_text(entry, ""); 
        gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_PRIMARY, "open-menu-symbolic"); 
        gtk_entry_set_icon_activatable(entry, GTK_ENTRY_ICON_PRIMARY, FALSE);
    }
}

void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer user_data) {
    GtkEntry* entry = GTK_ENTRY(g_object_get_data(G_OBJECT(web_view), "entry"));
    if (!entry) return;

    if (load_event == WEBKIT_LOAD_COMMITTED) {
        update_url_bar(web_view, entry);
    }
    
    if (load_event == WEBKIT_LOAD_FINISHED) {
        const char* uri = webkit_web_view_get_uri(web_view);
        if (uri) {
            update_url_bar(web_view, entry); 
            try_autofill(web_view, uri);
        }
    }
}

void close_tab_with_history(GtkWidget* widget, GtkNotebook* nb, int page_num) {
    GtkWidget* page = gtk_notebook_get_nth_page(nb, page_num);
    GList* children = gtk_container_get_children(GTK_CONTAINER(page));
    
    for(GList* l = children; l; l = l->next) {
        if(WEBKIT_IS_WEB_VIEW(l->data)) {
            const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(l->data));
            if (uri) {
                closed_tabs_history.push_back(std::string(uri));
                if (closed_tabs_history.size() > 20) closed_tabs_history.erase(closed_tabs_history.begin());
            }
            break;
        }
    }
    g_list_free(children);
    
    gtk_notebook_remove_page(nb, page_num);
    if (gtk_notebook_get_n_pages(nb) == 0) gtk_widget_destroy(widget);

#ifdef __linux__
    // Reclaim freed memory back to the OS immediately after a tab closes.
    // Without this, glibc holds onto freed heap memory speculatively.
    malloc_trim(0);
#endif
}

static void on_tab_close(GtkButton*, gpointer v_box_widget) { 
    GtkWidget* win = GTK_WIDGET(g_object_get_data(G_OBJECT(v_box_widget), "win"));
    GtkNotebook* nb = get_notebook(win);
    int page_num = gtk_notebook_page_num(nb, GTK_WIDGET(v_box_widget));
    if (page_num != -1) {
        close_tab_with_history(win, nb, page_num);
    }
}

static gboolean on_decide_policy(WebKitWebView* v, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer win) {
    switch (type) {
        case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION: {
            WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            if (webkit_navigation_action_get_mouse_button(action) == 2) {
                WebKitURIRequest* req = webkit_navigation_action_get_request(action);
                create_new_tab(GTK_WIDGET(win), webkit_uri_request_get_uri(req), global_context, nullptr, false);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            break;
        }
        case WEBKIT_POLICY_DECISION_TYPE_RESPONSE: {
            WebKitResponsePolicyDecision* r_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
            if (!webkit_response_policy_decision_is_mime_type_supported(r_decision)) {
                webkit_policy_decision_download(decision);
                return TRUE;
            }
            break;
        }
        default: break;
    }
    return FALSE;
}

static GtkWidget* on_web_view_create(WebKitWebView* web_view, WebKitNavigationAction* navigation_action, gpointer user_data) {
    GtkWidget* win = GTK_WIDGET(user_data);
    return create_new_tab(win, "", global_context, web_view);
}

void show_site_info_popover(GtkEntry* entry, WebKitWebView* view) {
    const char* uri = webkit_web_view_get_uri(view);
    bool is_secure = (uri && strncmp(uri, "https://", 8) == 0);

    GtkWidget* popover = gtk_popover_new(GTK_WIDGET(entry));
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    
    GdkRectangle rect;
    gtk_entry_get_icon_area(entry, GTK_ENTRY_ICON_PRIMARY, &rect);
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(box, "margin", 10, NULL);

    GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    const char* icon_name = is_secure ? "channel-secure-symbolic" : "channel-insecure-symbolic";
    const char* text = is_secure ? "Connection is secure" : "Connection is not secure";
    
    gtk_box_pack_start(GTK_BOX(status_box), gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_box), gtk_label_new(text), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), status_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    
    GtkWidget* cookies_btn = gtk_button_new_with_label("Cookies and site data");
    gtk_button_set_relief(GTK_BUTTON(cookies_btn), GTK_RELIEF_NONE);
    gtk_box_pack_start(GTK_BOX(box), cookies_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void on_icon_press(GtkEntry *entry, GtkEntryIconPosition icon_pos, GdkEvent *event, gpointer user_data) {
    if (icon_pos == GTK_ENTRY_ICON_PRIMARY) {
        WebKitWebView* view = WEBKIT_WEB_VIEW(user_data);
        show_site_info_popover(entry, view);
    }
}

void show_menu(GtkButton* btn, gpointer win) {
    GtkWidget* popover = gtk_popover_new(GTK_WIDGET(btn));
    gtk_style_context_add_class(gtk_widget_get_style_context(popover), "menu-popover");
    
    GtkWidget* menu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    
auto create_base_btn = [&](const char* label, const char* icon_name) {
        GtkWidget* btn = gtk_button_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "menu-item");
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        
        GtkWidget* img = create_menu_icon(icon_name);
        
        GtkWidget* lbl = gtk_label_new(label);
        
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 10);
        gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(btn), box);
        return btn;
    };

    struct MenuAction { GtkWidget* win; std::string url; };

    auto mk_url_item = [&](const char* label, const char* icon, const std::string& url) {
        GtkWidget* b = create_base_btn(label, icon);
        
        MenuAction* action = new MenuAction{ (GtkWidget*)win, url };

        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
            MenuAction* a = (MenuAction*)data;
            WebKitWebView* active = get_active_webview(a->win);
            WebKitWebContext* ctx = active ? webkit_web_view_get_context(active) : global_context;
            
            create_new_tab(a->win, a->url, ctx);
            delete a; 
        }), action);
        return b;
    };

    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("New Tab", "tab-new-symbolic", settings.home_url), FALSE, FALSE, 0);
    GtkWidget* incognito_btn = create_base_btn("New Incognito Window", "user-invisible-symbolic");
    g_signal_connect(incognito_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
        WebKitWebContext* ephemeral_ctx = webkit_web_context_new_ephemeral();
        create_window(ephemeral_ctx);
    }), NULL);
    gtk_box_pack_start(GTK_BOX(menu_box), incognito_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("Downloads", "folder-download-symbolic", settings.downloads_url), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("Passwords", "dialog-password-symbolic", settings.passwords_url), FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("History", "document-open-recent-symbolic", settings.history_url), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("Settings", "preferences-system-symbolic", settings.settings_url), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("Task Manager", "utilities-system-monitor-symbolic", settings.task_manager_url), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(menu_box), mk_url_item("About Zyro", "help-about-symbolic", settings.about_url), FALSE, FALSE, 0); 
    gtk_box_pack_start(GTK_BOX(menu_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    
    GtkWidget* exit_btn = create_base_btn("Exit Zyro", "application-exit-symbolic");
    g_signal_connect(exit_btn, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_box_pack_start(GTK_BOX(menu_box), exit_btn, FALSE, FALSE, 0);

    gtk_widget_show_all(menu_box);
    gtk_container_add(GTK_CONTAINER(popover), menu_box);
    gtk_popover_popup(GTK_POPOVER(popover));
}

void create_blocker_popover(GtkWidget* toggle_btn) {
    GtkWidget* popover = gtk_popover_new(toggle_btn);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(box, "margin", 15, "width-request", 220, NULL);
    
    GtkWidget* title = gtk_label_new("<b>AdShield</b>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    
    GtkWidget* switch_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* sw_label = gtk_label_new("Enable Protection");
    GtkWidget* sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), global_blocker_enabled);
    
    g_signal_connect(sw, "state-set", G_CALLBACK(+[](GtkSwitch* s, gboolean state, gpointer data){
        toggle_blocker();
        GtkWidget* btn = (GtkWidget*)data;
        GtkWidget* img = gtk_bin_get_child(GTK_BIN(btn));
        if (state) {
            gtk_image_set_from_icon_name(GTK_IMAGE(img), "security-high-symbolic", GTK_ICON_SIZE_BUTTON);
            gtk_widget_set_tooltip_text(btn, "AdShield: ON");
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(img), "security-low-symbolic", GTK_ICON_SIZE_BUTTON);
            gtk_widget_set_tooltip_text(btn, "AdShield: OFF");
        }
        return FALSE; 
    }), toggle_btn);

    gtk_box_pack_start(GTK_BOX(switch_box), sw_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(switch_box), sw, FALSE, FALSE, 0);
    
    GtkWidget* stats_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    
    GtkWidget* stats_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(stats_grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(stats_grid), 5);
    
    GtkWidget* l_count = gtk_label_new("Rules Active:");
    GtkWidget* v_count = gtk_label_new("12"); 
    gtk_label_set_xalign(GTK_LABEL(l_count), 0);
    gtk_label_set_xalign(GTK_LABEL(v_count), 1);
    
    GtkWidget* l_blocked = gtk_label_new("Blocked:");
    std::string count_str = std::to_string(get_blocked_count());
    GtkWidget* v_blocked = gtk_label_new(count_str.c_str());
    gtk_label_set_xalign(GTK_LABEL(l_blocked), 0);
    gtk_label_set_xalign(GTK_LABEL(v_blocked), 1);

    gtk_grid_attach(GTK_GRID(stats_grid), l_count, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), v_count, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), l_blocked, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stats_grid), v_blocked, 1, 1, 1, 1);
    
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), switch_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), stats_sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), stats_grid, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);
    
    g_object_set_data(G_OBJECT(toggle_btn), "popover", popover);
    
    g_signal_connect(toggle_btn, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer data){
        GtkWidget* pop = (GtkWidget*)data;
        if(gtk_toggle_button_get_active(b)) {
             gtk_popover_popup(GTK_POPOVER(pop));
        } else {
             gtk_popover_popdown(GTK_POPOVER(pop));
        }
    }), popover);
    
    g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover*, gpointer b){
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), FALSE);
    }), toggle_btn);
}

GtkWidget* create_new_tab(GtkWidget* win, const std::string& url, WebKitWebContext* context, WebKitWebView* related_view, bool switch_to) {   
    GtkNotebook* notebook = get_notebook(win);
    
    WebKitUserContentManager* ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "zyro");
    
    GtkWidget* view;
    if (related_view) {
        view = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", ucm, "related-view", related_view, NULL));
    } else {
        view = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", ucm, NULL));
    }

    WebKitSettings *wk_settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
    webkit_settings_set_enable_developer_extras(wk_settings, TRUE); // Required for F12 inspector

    // Disable bfcache — keeps full page renders in memory for back/forward.
    // Not worth the RAM cost for most browsing.
    webkit_settings_set_enable_page_cache(wk_settings, FALSE);

    webkit_settings_set_enable_site_specific_quirks(wk_settings, FALSE);

    webkit_settings_set_enable_webgl(wk_settings, TRUE);

    // Don't hold camera/mic hardware open on every tab by default.
    webkit_settings_set_enable_media_stream(wk_settings, FALSE);

    webkit_settings_set_enable_smooth_scrolling(wk_settings, TRUE);

    // Suppress per-message stdout allocations; pages should use devtools, not stdout.
    webkit_settings_set_enable_write_console_messages_to_stdout(wk_settings, FALSE);

    // ON_DEMAND: GPU resources are only allocated when the page actually uses
    // hardware acceleration (video, canvas, WebGL). ALWAYS wastes GPU memory
    // on every plain text/HTML page. NEVER breaks YouTube. ON_DEMAND is the
    // sweet spot — WebKit will still use the GPU for video automatically.
    if (settings.hardware_acceleration) {
        webkit_settings_set_hardware_acceleration_policy(wk_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
    } else {
        webkit_settings_set_hardware_acceleration_policy(wk_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    }

    g_signal_connect(view, "user-message-received", G_CALLBACK(on_user_message_received), NULL);

    g_signal_connect(ucm, "script-message-received::zyro", G_CALLBACK(on_script_message), view);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");

    auto mkbtn = [](const char* icon, const char* tooltip) { 
        GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON); 
        gtk_widget_set_tooltip_text(b, tooltip); return b; 
    };
    
    GtkWidget* b_back = mkbtn("go-previous-symbolic", "Back");
    GtkWidget* b_fwd = mkbtn("go-next-symbolic", "Forward");
    GtkWidget* b_refresh = mkbtn("view-refresh-symbolic", "Reload");
    GtkWidget* b_home = mkbtn("go-home-symbolic", "Home");
    GtkWidget* b_menu = mkbtn("open-menu-symbolic", "Menu"); 

    GtkWidget* b_media = mkbtn("media-playback-start-symbolic", "Media Control");
    // Connect to the singleton handler — no per-tab popover creation.
    g_signal_connect(b_media, "clicked", G_CALLBACK(on_media_btn_clicked), NULL);

    GtkWidget* b_downloads = gtk_toggle_button_new(); 
    GtkWidget* dl_icon = gtk_image_new_from_icon_name("folder-download-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(b_downloads), dl_icon);
    gtk_widget_set_tooltip_text(b_downloads, "Downloads");
    
    GtkWidget* dl_popover = gtk_popover_new(b_downloads);
    gtk_popover_set_position(GTK_POPOVER(dl_popover), GTK_POS_BOTTOM);
    
    GtkWidget* pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    g_object_set(pop_box, "margin", 10, "width-request", 300, NULL);
    
    GtkWidget* pop_header = gtk_label_new("<b>Downloads</b>");
    gtk_label_set_use_markup(GTK_LABEL(pop_header), TRUE);
    gtk_label_set_xalign(GTK_LABEL(pop_header), 0);
    
    GtkWidget* list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* full_page_link = gtk_button_new_with_label("View All Downloads");
    gtk_button_set_relief(GTK_BUTTON(full_page_link), GTK_RELIEF_NONE);
    g_signal_connect(full_page_link, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
        if(global_window && global_context) {
            create_new_tab(global_window, settings.downloads_url, global_context);
            if(global_downloads_popover) gtk_popover_popdown(GTK_POPOVER(global_downloads_popover));
        }
    }), NULL);

    GtkWidget* b_blocker = gtk_toggle_button_new();
    GtkWidget* bl_icon = gtk_image_new_from_icon_name(
        global_blocker_enabled ? "security-high-symbolic" : "security-low-symbolic", 
        GTK_ICON_SIZE_BUTTON
    );
    gtk_container_add(GTK_CONTAINER(b_blocker), bl_icon);
    gtk_widget_set_tooltip_text(b_blocker, global_blocker_enabled ? "AdShield: ON" : "AdShield: OFF");
    g_object_set_data(G_OBJECT(b_blocker), "type", (gpointer)"blocker_btn"); // For updates
    create_blocker_popover(b_blocker);

    // --- Create URL Entry and Completion ---
    GtkWidget* url_entry = gtk_entry_new();
    GtkEntryCompletion* completion = gtk_entry_completion_new();
    GtkListStore* completion_model = gtk_list_store_new(1, G_TYPE_STRING);
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(completion_model));
    gtk_entry_completion_set_text_column(completion, 0);
    g_object_unref(completion_model);
    gtk_entry_set_completion(GTK_ENTRY(url_entry), completion);
    gtk_entry_completion_set_minimum_key_length(completion, 2);
    // ---------------------------------------

    gtk_box_pack_start(GTK_BOX(toolbar), b_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_fwd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_home, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(toolbar), url_entry, TRUE, TRUE, 5);
    gtk_box_pack_start(GTK_BOX(toolbar), b_downloads, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_blocker, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_media, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b_menu, FALSE, FALSE, 0);

    GtkWidget* bookmarks_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(bookmarks_bar), "bookmarks-bar");
    refresh_bookmarks_bar(bookmarks_bar);

    GtkWidget* page_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(page_box), "win", win); 
    g_object_set_data(G_OBJECT(page_box), "bookmarks_bar", bookmarks_bar); 
    
    gtk_box_pack_start(GTK_BOX(page_box), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_box), bookmarks_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_box), view, TRUE, TRUE, 0);

    GtkWidget* tab_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget* label = gtk_label_new("New Tab");
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_name(close, "tab-close-btn");
    gtk_box_pack_start(GTK_BOX(tab_header), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab_header), close, FALSE, FALSE, 0);
    gtk_widget_show_all(tab_header);
    
    gtk_widget_set_has_tooltip(tab_header, TRUE);
    g_signal_connect(tab_header, "query-tooltip", G_CALLBACK(+[](GtkWidget* w, gint x, gint y, gboolean k, GtkTooltip* tooltip, gpointer v_ptr){
        WebKitWebView* view = WEBKIT_WEB_VIEW(v_ptr);
        
        std::string total_mem = get_total_memory_str();
        
        int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "web_pid"));
        std::string tab_mem = "Calculating...";
        
        if (pid > 0) {
            long kb = get_pid_rss_kb(pid);
            if (kb > 0) {
                std::stringstream ss;
                ss << std::fixed << std::setprecision(1) << (kb / 1024.0) << " MB";
                tab_mem = ss.str();
            } else {
                tab_mem = "0 MB (Sleeping/Shared)";
            }
        } else {
            tab_mem = "Waiting for process...";
        }

        std::string tip = "Tab Memory: " + tab_mem + "\nTotal Zyro: " + total_mem + "\nPID: " + std::to_string(pid);
        
        gtk_tooltip_set_text(tooltip, tip.c_str());
        return TRUE;
    }), view);

    g_signal_connect(b_back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ webkit_web_view_go_back(WEBKIT_WEB_VIEW(v)); }), view);
    g_signal_connect(b_fwd, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ webkit_web_view_go_forward(WEBKIT_WEB_VIEW(v)); }), view);
    g_signal_connect(b_refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ webkit_web_view_reload(WEBKIT_WEB_VIEW(v)); }), view);
    g_signal_connect(b_home, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ webkit_web_view_load_uri(WEBKIT_WEB_VIEW(v), settings.home_url.c_str()); }), view);
    
    g_object_set_data(G_OBJECT(view), "entry", url_entry);
    g_object_set_data(G_OBJECT(view), "page_box", page_box);

    g_signal_connect(url_entry, "changed", G_CALLBACK(on_url_changed), NULL);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_activate), view);
    g_signal_connect(url_entry, "icon-press", G_CALLBACK(on_icon_press), view); 
    g_signal_connect(completion, "match-selected", G_CALLBACK(on_match_selected), url_entry);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(url_entry), GTK_ENTRY_ICON_SECONDARY, "non-starred-symbolic");
    g_signal_connect(url_entry, "icon-press", G_CALLBACK(+[](GtkEntry* entry, GtkEntryIconPosition pos, GdkEvent* ev, gpointer v_ptr){
        if (pos == GTK_ENTRY_ICON_SECONDARY) {
            WebKitWebView* view = WEBKIT_WEB_VIEW(v_ptr);
            std::string url = webkit_web_view_get_uri(view);
            const char* title = webkit_web_view_get_title(view);
            if(url.empty()) return;

            auto it = std::find_if(bookmarks_list.begin(), bookmarks_list.end(), 
                [&](const BookmarkItem& b){ return b.url == url; });
            
            if (it != bookmarks_list.end()) {
                return; 
            }

            bookmarks_list.push_back({ title ? title : url, url });
            save_bookmarks_to_disk();
            
            gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "starred-symbolic");
            
            GtkWidget* p_box = (GtkWidget*)g_object_get_data(G_OBJECT(view), "page_box");
            GtkWidget* b_bar = (GtkWidget*)g_object_get_data(G_OBJECT(p_box), "bookmarks_bar");
            refresh_bookmarks_bar(b_bar);
        }
    }), view);

    g_signal_connect(view, "notify::uri", G_CALLBACK(+[](WebKitWebView* v, GParamSpec*, gpointer){ 
        GtkEntry* e = GTK_ENTRY(g_object_get_data(G_OBJECT(v), "entry"));
        update_url_bar(v, e);
    }), NULL);

    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), win);
    g_signal_connect(view, "create", G_CALLBACK(on_web_view_create), win);
    
    g_signal_connect(view, "notify::title", G_CALLBACK(+[](WebKitWebView* v, GParamSpec*, GtkLabel* l){ 
        const char* t = webkit_web_view_get_title(v); if(t) gtk_label_set_text(l, t); 
    }), label);

    g_signal_connect(b_menu, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer w){ show_menu(b, w); }), win);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), page_box);

    int page = gtk_notebook_append_page(notebook, page_box, tab_header);
    gtk_notebook_set_tab_reorderable(notebook, page_box, TRUE);
    gtk_widget_show_all(page_box);
    
    if (switch_to) {
        gtk_notebook_set_current_page(notebook, page);
    }
    
    if (!url.empty()) {
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), url.c_str());
    }
    
    return view;
}

void show_search_overlay(GtkWidget* parent_win) {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(parent_win));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
    
    // STRICTLY set the size so it doesn't grow
    gtk_window_set_default_size(GTK_WINDOW(win), 600, 400);
    gtk_widget_set_size_request(win, 600, 400);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    
    gtk_widget_set_name(win, "search-overlay-window");

    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main_box), 15);
    gtk_container_add(GTK_CONTAINER(win), main_box);

    // --- Search Entry ---
    GtkWidget* entry = gtk_entry_new();
    gtk_widget_set_name(entry, "search-overlay-entry");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search or enter URL...");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_PRIMARY, "system-search-symbolic");

    PangoAttrList* attr_list = pango_attr_list_new();
    pango_attr_list_insert(attr_list, pango_attr_size_new(16 * PANGO_SCALE));
    gtk_entry_set_attributes(GTK_ENTRY(entry), attr_list);
    pango_attr_list_unref(attr_list);

    gtk_box_pack_start(GTK_BOX(main_box), entry, FALSE, FALSE, 0);

    // --- Results List ---
    GtkWidget* scrolled = gtk_scrolled_window_new(NULL, NULL);
    // POLICY_NEVER for horizontal ensures it won't scroll sideways, forcing the ellipsize logic to kick in
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    
    GtkListStore* store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget* tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_view), FALSE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree_view), FALSE);
    
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, 
        "height", 30, 
        "xpad", 10, 
        "ellipsize", PANGO_ELLIPSIZE_END,
        NULL
    );

    GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes("Result", renderer, "text", 0, NULL);
    gtk_tree_view_column_set_expand(col, TRUE); 
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);

    gtk_container_add(GTK_CONTAINER(scrolled), tree_view);
    gtk_box_pack_start(GTK_BOX(main_box), scrolled, TRUE, TRUE, 0);

    g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer data){
        GtkListStore* s = GTK_LIST_STORE(data);
        std::string text = gtk_entry_get_text(GTK_ENTRY(e));
        if(text.length() > 0 && text.find("://") == std::string::npos) {
            fetch_suggestions(text, true, s);
        } else {
            gtk_list_store_clear(s);
        }
    }), store);

    g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer win_ptr){
        GtkWidget* win = GTK_WIDGET(win_ptr);
        std::string text = gtk_entry_get_text(e);
        
        if (!text.empty()) {
            std::string t = text;
            bool is_url = false;
            if (t.find("://") != std::string::npos) is_url = true;
            else if (t.find(".") != std::string::npos && t.find(" ") == std::string::npos) is_url = true;
            else if (t.find("localhost") != std::string::npos) is_url = true;

            std::string final_url = t;
            if (!is_url) {
                final_url = settings.search_engine + t;
                add_search_query(t);
            } else if (t.find("://") == std::string::npos) {
                final_url = "https://" + t;
            }

            WebKitWebView* v = get_active_webview(global_window);
            if (v) webkit_web_view_load_uri(v, final_url.c_str());
        }
        gtk_widget_destroy(win);
    }), win);

    g_signal_connect(tree_view, "row-activated", G_CALLBACK(+[](GtkTreeView* t, GtkTreePath* p, GtkTreeViewColumn*, gpointer win_ptr){
        GtkTreeModel* model = gtk_tree_view_get_model(t);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, p)) {
            gchar* value;
            gtk_tree_model_get(model, &iter, 0, &value, -1);
            std::string url(value);
            g_free(value);

            std::string t_str = url;
            bool is_url = false;
            if (t_str.find("://") != std::string::npos) is_url = true;
            else if (t_str.find(".") != std::string::npos && t_str.find(" ") == std::string::npos) is_url = true;

            std::string final_url = t_str;
            if (!is_url) {
                final_url = settings.search_engine + t_str;
                add_search_query(t_str);
            } else if (t_str.find("://") == std::string::npos) {
                final_url = "https://" + t_str;
            }

            WebKitWebView* v = get_active_webview(global_window);
            if (v) webkit_web_view_load_uri(v, final_url.c_str());
            
            gtk_widget_destroy(GTK_WIDGET(win_ptr));
        }
    }), win);

    g_signal_connect(entry, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer list_ptr) -> gboolean {
        if (ev->keyval == GDK_KEY_Down) {
            gtk_widget_grab_focus(GTK_WIDGET(list_ptr));
            return TRUE;
        }
        return FALSE;
    }), tree_view);

    g_signal_connect(win, "key-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventKey* event, gpointer) -> gboolean {
        if (event->keyval == GDK_KEY_Escape) {
            gtk_widget_destroy(w);
            return TRUE;
        }
        return FALSE;
    }), NULL);

    g_signal_connect(win, "focus-out-event", G_CALLBACK(+[](GtkWidget* w, GdkEventFocus*, gpointer) -> gboolean {
        gtk_widget_destroy(w);
        return FALSE;
    }), NULL);

    gtk_widget_show_all(win);
}

gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    WebKitWebView* view = get_active_webview(widget);
    GtkNotebook* nb = get_notebook(widget);
    GtkEntry* url_entry = nullptr;
    if (view) url_entry = GTK_ENTRY(g_object_get_data(G_OBJECT(view), "entry"));

    if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK)) {
        switch (event->keyval) {
            case GDK_KEY_t: 
            case GDK_KEY_T:
                if (!closed_tabs_history.empty()) {
                    std::string last_url = closed_tabs_history.back();
                    closed_tabs_history.pop_back();
                    create_new_tab(widget, last_url, global_context);
                } 
                else if (!last_session_urls.empty()) {
                    for (const auto& u : last_session_urls) {
                        create_new_tab(widget, u, global_context);
                    }
                    last_session_urls.clear();
                }
                return TRUE;

            case GDK_KEY_Tab:
            case GDK_KEY_ISO_Left_Tab:
                if (nb) {
                    int curr = gtk_notebook_get_current_page(nb);
                    int last = gtk_notebook_get_n_pages(nb) - 1;
                    gtk_notebook_set_current_page(nb, (curr == 0) ? last : curr - 1);
                }
                return TRUE;
        }
    }

    if ((event->state & GDK_CONTROL_MASK) && !(event->state & GDK_SHIFT_MASK)) {
        if (event->keyval >= GDK_KEY_1 && event->keyval <= GDK_KEY_8) {
            int page_idx = event->keyval - GDK_KEY_1; 
            if (page_idx < gtk_notebook_get_n_pages(nb)) {
                gtk_notebook_set_current_page(nb, page_idx);
            }
            return TRUE;
        }
        if (event->keyval == GDK_KEY_9) {
            gtk_notebook_set_current_page(nb, gtk_notebook_get_n_pages(nb) - 1);
            return TRUE;
        }
    }


    if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_MOD1_MASK)) {
        if (event->keyval == GDK_KEY_l || event->keyval == GDK_KEY_L) {
            if (view) {
                const char* uri = webkit_web_view_get_uri(view);
                if (uri && std::string(uri).find("home.html") != std::string::npos) {
                    run_js(view, "document.getElementById('search').focus(); document.getElementById('search').select();");
                } 
                else {
                    show_search_overlay(widget);
                }
            } else {
                show_search_overlay(widget);
            }
            return TRUE;
        }
    }

    if (event->state & GDK_CONTROL_MASK) {
        switch (event->keyval) {
            case GDK_KEY_t: 
                create_new_tab(widget, settings.home_url, global_context); 
                return TRUE;
            case GDK_KEY_w: {
                int page = gtk_notebook_get_current_page(nb);
                if (page != -1) {
                    close_tab_with_history(widget, nb, page);
                }
                return TRUE;
            }
            case GDK_KEY_r: 
            case GDK_KEY_F5:
                if (view) webkit_web_view_reload(view);
                return TRUE;
            case GDK_KEY_l: 
                if (url_entry) gtk_widget_grab_focus(GTK_WIDGET(url_entry));
                return TRUE;
            case GDK_KEY_Tab: 
            case GDK_KEY_Page_Down: 
                if (nb) {
                    int curr = gtk_notebook_get_current_page(nb);
                    int last = gtk_notebook_get_n_pages(nb) - 1;
                    gtk_notebook_set_current_page(nb, (curr == last) ? 0 : curr + 1);
                }
                return TRUE;
            case GDK_KEY_Page_Up: 
                if (nb) {
                    int curr = gtk_notebook_get_current_page(nb);
                    int last = gtk_notebook_get_n_pages(nb) - 1;
                    gtk_notebook_set_current_page(nb, (curr == 0) ? last : curr - 1);
                }
                return TRUE;
        }
    }

    if (event->state & GDK_MOD1_MASK) {
        switch (event->keyval) {
            case GDK_KEY_Left:
                if (view && webkit_web_view_can_go_back(view)) webkit_web_view_go_back(view);
                return TRUE;
            case GDK_KEY_Right:
                if (view && webkit_web_view_can_go_forward(view)) webkit_web_view_go_forward(view);
                return TRUE;
        }
    }
    
    if (event->keyval == GDK_KEY_F5 && view) {
        webkit_web_view_reload(view);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_F12 && view) {
        WebKitWebInspector *inspector = webkit_web_view_get_inspector(view);
        webkit_web_inspector_show(inspector);
        return TRUE;
    }

    return FALSE; 
}
void create_window(WebKitWebContext* ctx) {
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    bool is_incognito = webkit_web_context_is_ephemeral(ctx);

    if (!is_incognito) {
        global_window = win;
        // Preload session into memory
        load_session_into_memory();
        
        // Save session on close (and fix WebKit exit crash by stopping loads)
        g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer) -> gboolean {
            save_session_to_disk(w);
            return FALSE; // Propagate to destroy
        }), NULL);
        
        g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    } else {
        gtk_window_set_title(GTK_WINDOW(win), "Zyro (Incognito)");
        gtk_style_context_add_class(gtk_widget_get_style_context(win), "incognito");
        // Don't save session for incognito, but standard destroy
        g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    }

    if (!is_incognito) gtk_window_set_title(GTK_WINDOW(win), "Zyro");
    
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 800);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), NULL);
    
    std::string style_path = get_assets_path() + "style.css";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, style_path.c_str(), NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    
    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(nb), FALSE);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
    g_object_set_data(G_OBJECT(win), "notebook", nb);

    GtkWidget* add_tab_btn = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(add_tab_btn, "New Tab");
    gtk_widget_show(add_tab_btn);

    gtk_notebook_set_action_widget(GTK_NOTEBOOK(nb), add_tab_btn, GTK_PACK_END);

    g_signal_connect(add_tab_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        GtkWidget* w = GTK_WIDGET(data);
        
        WebKitWebView* v = get_active_webview(w); 
        
        WebKitWebContext* c = v ? webkit_web_view_get_context(v) : global_context;
        
        create_new_tab(w, settings.home_url, c, v); 
    }), win);

    gtk_box_pack_start(GTK_BOX(box), nb, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), box);


    if (!webkit_web_context_is_ephemeral(ctx)) {
        g_timeout_add_seconds(10, auto_save_data, NULL); 
    }

    // -----------------------------------------------------------------------
    // BACKGROUND TAB THROTTLING via the Page Visibility API.
    //
    // When WebKit renders a tab that isn't the active one, the GTK widget is
    // still in the widget tree and the renderer process runs at full speed.
    // Well-behaved pages (YouTube, SPAs, animated dashboards) listen for the
    // standard 'visibilitychange' event and pause timers, animations, and
    // preloads when hidden — but WebKit only fires this automatically on
    // window minimise, not on notebook page switches.
    //
    // This signal fires the event manually so pages self-throttle the moment
    // you switch away.  The active tab always gets 'visible'; every other tab
    // gets 'hidden'.  We patch document.visibilityState to match so pages that
    // read the property (not just listen to the event) also behave correctly.
    // -----------------------------------------------------------------------
    g_signal_connect(nb, "switch-page",
        G_CALLBACK(+[](GtkNotebook* nb2, GtkWidget*, guint new_page, gpointer) {
            int n = gtk_notebook_get_n_pages(nb2);
            for (int i = 0; i < n; i++) {
                GtkWidget* page = gtk_notebook_get_nth_page(nb2, i);
                GList* children = gtk_container_get_children(GTK_CONTAINER(page));
                for (GList* l = children; l; l = l->next) {
                    if (WEBKIT_IS_WEB_VIEW(l->data)) {
                        WebKitWebView* v = WEBKIT_WEB_VIEW(l->data);
                        bool is_active = ((int)new_page == i);
                        // Patch visibilityState so document.visibilityState
                        // reads correctly, then fire the standard event.
                        std::string js = is_active
                            ? "Object.defineProperty(document,'visibilityState',{get:()=>'visible',configurable:true});"
                              "document.dispatchEvent(new Event('visibilitychange'));"
                            : "Object.defineProperty(document,'visibilityState',{get:()=>'hidden',configurable:true});"
                              "document.dispatchEvent(new Event('visibilitychange'));";
                        run_js(v, js);
                        break;
                    }
                }
                g_list_free(children);
            }
        }), NULL);

    gtk_widget_show_all(win);
    
    create_new_tab(win, settings.home_url, ctx);
}


void send_media_command(WebKitWebView* view, const std::string& command) {
    std::string script = 
        "var vid = document.querySelector('video, audio');"
        "if (vid) {"
        "  if ('" + command + "' === 'playpause') { vid.paused ? vid.play() : vid.pause(); }"
        "  else if ('" + command + "' === 'next') { vid.currentTime += 10; }"
        "  else if ('" + command + "' === 'back') { vid.currentTime -= 10; }"
        "}";
    run_js(view, script);
}


void update_media_popup() {
    if (!global_media_list_box) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(global_media_list_box));
    for(GList* it = children; it; it = it->next) gtk_widget_destroy(GTK_WIDGET(it->data));
    g_list_free(children);

    GtkNotebook* nb = get_notebook(global_window);
    int n_pages = gtk_notebook_get_n_pages(nb);
    bool found_media = false;

    for (int i = 0; i < n_pages; i++) {
        GtkWidget* page_box = gtk_notebook_get_nth_page(nb, i);
        WebKitWebView* view = nullptr;
        
        GList* box_children = gtk_container_get_children(GTK_CONTAINER(page_box));
        for (GList* l = box_children; l != NULL; l = l->next) {
            if (WEBKIT_IS_WEB_VIEW(l->data)) {
                view = WEBKIT_WEB_VIEW(l->data);
                break;
            }
        }
        g_list_free(box_children);

        // Improved Check: WebKit flag OR Title contains common media keywords if URI is YouTube
        const char* uri = webkit_web_view_get_uri(view);
        bool is_potential_media = (uri && (std::string(uri).find("youtube.com") != std::string::npos || 
                                         std::string(uri).find("spotify.com") != std::string::npos));

        if (view && (webkit_web_view_is_playing_audio(view) || is_potential_media)) {
            found_media = true;
            const char* title = webkit_web_view_get_title(view);

            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_style_context_add_class(gtk_widget_get_style_context(row), "media-row");

            GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            GtkWidget* title_lbl = gtk_label_new(title ? title : "Active Media");
            gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
            gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "media-title");
            
            gtk_box_pack_start(GTK_BOX(header_box), gtk_image_new_from_icon_name("audio-x-generic-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(header_box), title_lbl, TRUE, TRUE, 0);

            GtkWidget* ctrl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_style_context_add_class(gtk_widget_get_style_context(ctrl_box), "media-controls");
            
            GtkWidget* b_back = gtk_button_new_from_icon_name("media-seek-backward-symbolic", GTK_ICON_SIZE_BUTTON);
            GtkWidget* b_pp = gtk_button_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
            GtkWidget* b_next = gtk_button_new_from_icon_name("media-seek-forward-symbolic", GTK_ICON_SIZE_BUTTON);
            GtkWidget* b_goto = gtk_button_new_from_icon_name("go-jump-symbolic", GTK_ICON_SIZE_BUTTON);
            
            g_signal_connect(b_pp, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ send_media_command((WebKitWebView*)v, "playpause"); }), view);
            g_signal_connect(b_back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ send_media_command((WebKitWebView*)v, "back"); }), view);
            g_signal_connect(b_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer v){ send_media_command((WebKitWebView*)v, "next"); }), view);
            
            struct TabJump { GtkNotebook* n; int p; };
            TabJump* jump = new TabJump{ nb, i };
            g_signal_connect(b_goto, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
                TabJump* d = (TabJump*)data;
                gtk_notebook_set_current_page(d->n, d->p);
                gtk_popover_popdown(GTK_POPOVER(global_media_popover));
                delete d;
            }), jump);

            gtk_box_pack_start(GTK_BOX(ctrl_box), b_back, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(ctrl_box), b_pp, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(ctrl_box), b_next, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(ctrl_box), b_goto, TRUE, TRUE, 0);

            gtk_box_pack_start(GTK_BOX(row), header_box, FALSE, FALSE, 5);
            gtk_box_pack_start(GTK_BOX(row), ctrl_box, FALSE, FALSE, 5);
            
            gtk_container_add(GTK_CONTAINER(global_media_list_box), row);
        }
    }

    if (!found_media) {
        GtkWidget* empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty_box), "media-empty-state");
        
        GtkWidget* icon = gtk_image_new_from_icon_name("audio-volume-muted-symbolic", GTK_ICON_SIZE_DIALOG);
        GtkWidget* label = gtk_label_new("No media playing");
        
        gtk_box_pack_start(GTK_BOX(empty_box), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(empty_box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(global_media_list_box), empty_box);
    }
    gtk_widget_show_all(global_media_list_box);
}

// ---------------------------------------------------------------------------
// Media popover — SINGLETON.
//
// Previously create_media_player_popover() was called inside create_new_tab(),
// creating a fresh GtkPopover widget for every tab and overwriting
// global_media_popover each time.  This meant:
//   a) N orphaned popovers (one per non-last tab) that the refresh timer
//      could never see as visible, so live-updating was broken on all but
//      the last-opened tab.
//   b) Extra GTK widget allocation on every new tab.
//
// Fix: create the popover exactly once (lazily, on first click of any media
// button), then re-parent it to whichever button was just clicked via
// gtk_popover_set_relative_to().  Every tab's media button shares the one
// global popover.
// ---------------------------------------------------------------------------
static void on_media_btn_clicked(GtkButton* btn, gpointer) {
    if (!global_media_popover) {
        // First ever click — build the popover widget tree once.
        global_media_popover = gtk_popover_new(GTK_WIDGET(btn));
        gtk_popover_set_position(GTK_POPOVER(global_media_popover), GTK_POS_BOTTOM);
        g_object_add_weak_pointer(G_OBJECT(global_media_popover),
                                  (gpointer*)&global_media_popover);

        GtkWidget* outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_size_request(outer_box, 300, 150);
        g_object_set(outer_box, "margin", 12, NULL);

        GtkWidget* title = gtk_label_new("<b>Global Media Control</b>");
        gtk_label_set_use_markup(GTK_LABEL(title), TRUE);

        global_media_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

        gtk_box_pack_start(GTK_BOX(outer_box), title, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(outer_box), global_media_list_box, TRUE, TRUE, 0);

        gtk_container_add(GTK_CONTAINER(global_media_popover), outer_box);
        gtk_widget_show_all(outer_box);
    } else {
        // Re-parent the existing popover to the button that was just clicked
        // so the arrow points at the right toolbar.
        gtk_popover_set_relative_to(GTK_POPOVER(global_media_popover),
                                    GTK_WIDGET(btn));
    }

    update_media_popup();
    gtk_popover_popup(GTK_POPOVER(global_media_popover));
}