#include <gtk/gtk.h>
#include "Globals.h"
#include "Security.h"
#include "Storage.h"
#include "Utils.h"
#include "Downloads.h"
#include "Browser.h"
#include "Blocker.h"

#ifdef __linux__
#include <malloc.h>
#endif


static gboolean memory_trim_timer(gpointer) {
#ifdef __linux__
    malloc_trim(0);
#endif
    return TRUE; 
}

static gboolean refresh_media_popup_timer(gpointer) {
    if (global_media_popover && GTK_IS_WIDGET(global_media_popover) && gtk_widget_get_visible(global_media_popover)) {
        update_media_popup();
    }
    return TRUE; 
}

static gboolean refresh_download_popup_timer(gpointer) {
    if (global_downloads_popover && GTK_IS_WIDGET(global_downloads_popover) && gtk_widget_get_visible(global_downloads_popover)) {
        update_downloads_popup();
    }
    return TRUE; 
}

static gboolean update_home_stats(gpointer data) {
    if(!global_window) return TRUE;
    GtkNotebook* notebook = get_notebook(global_window);
    if (!notebook) return TRUE;
    
    int pages = gtk_notebook_get_n_pages(notebook);
    for (int i=0; i<pages; i++) {
        GtkWidget* page_box = gtk_notebook_get_nth_page(notebook, i);
        GList* children = gtk_container_get_children(GTK_CONTAINER(page_box));
        for (GList* l = children; l != NULL; l = l->next) {
            if (WEBKIT_IS_WEB_VIEW(l->data)) {
                WebKitWebView* view = WEBKIT_WEB_VIEW(l->data);
                const char* uri = webkit_web_view_get_uri(view);
                if (uri && std::string(uri).find("home.html") != std::string::npos) {
                    int cpu; std::string ram;
                    get_sys_stats(cpu, ram);
                    std::string script = "updateStats('" + std::to_string(cpu) + "', '" + ram + "');";
                    run_js(view, script);
                }
                break;
            }
        }
        g_list_free(children);
    }
    return TRUE; 
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    
    std::string user_dir = get_user_data_dir();
    std::string cache = user_dir + "cache";
    std::string data = user_dir + "data";

    load_data();


    init_security(); 
    init_blocker();

    WebKitWebsiteDataManager* mgr = webkit_website_data_manager_new(
        "base-cache-directory", cache.c_str(), 
        "base-data-directory", data.c_str(), 
        NULL
    );

    global_context = webkit_web_context_new_with_website_data_manager(mgr);

    WebKitCacheModel model = (settings.cache_model == "web-browser") ? 
        WEBKIT_CACHE_MODEL_WEB_BROWSER : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER;
    webkit_web_context_set_cache_model(global_context, model);


    std::string extension_dir = get_self_path() + "/lib";
    webkit_web_context_set_web_extensions_directory(global_context, extension_dir.c_str());

    


    webkit_cookie_manager_set_persistent_storage(
        webkit_web_context_get_cookie_manager(global_context), 
        (data+"/cookies.sqlite").c_str(), 
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE
    );


    create_window(global_context);

    g_timeout_add(500, refresh_download_popup_timer, NULL);
    g_signal_connect(global_context, "download-started", G_CALLBACK(on_download_started), NULL);
    g_timeout_add(3000, update_home_stats, NULL);

    g_timeout_add(1000, refresh_media_popup_timer, NULL);

    if (settings.enable_memory_trim) {
        g_timeout_add_seconds(15, memory_trim_timer, NULL);
    }
    
    gtk_main();
    return 0;
}