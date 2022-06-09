/* SPDX-License-Identifier: Zlib */

#include "dbus-interface.h"
#include "synctex.h"
#include "macros.h"
#include "zathura.h"
#include "document.h"
#include "utils.h"
#include "adjustment.h"
#include "resources.h"

#include <girara/session.h>
#include <girara/utils.h>
#include <girara/settings.h>
#include <girara/commands.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

static const char DBUS_XML_FILENAME[] = "/org/pwmt/zathura/DBus/org.pwmt.zathura.xml";

static GBytes* load_xml_data(void)
{
  GResource* resource = zathura_resources_get_resource();
  if (resource != NULL) {
    return g_resource_lookup_data(resource, DBUS_XML_FILENAME,
                                  G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  }

  return NULL;
}

typedef struct private_s {
  zathura_t* zathura;
  GDBusNodeInfo* introspection_data;
  GDBusConnection* connection;
  guint owner_id;
  guint registration_id;
} ZathuraDbusPrivate;

G_DEFINE_TYPE_WITH_CODE(ZathuraDbus, zathura_dbus, G_TYPE_OBJECT, G_ADD_PRIVATE(ZathuraDbus))

/* template for bus name */
static const char DBUS_NAME_TEMPLATE[] = "org.pwmt.zathura.PID-%d";
/* object path */
static const char DBUS_OBJPATH[] = "/org/pwmt/zathura";
/* interface name */
static const char DBUS_INTERFACE[] = "org.pwmt.zathura";

static const GDBusInterfaceVTable interface_vtable;

static void
finalize(GObject* object)
{
  ZathuraDbus* dbus        = ZATHURA_DBUS(object);
  ZathuraDbusPrivate* priv = zathura_dbus_get_instance_private(dbus);

  if (priv->connection != NULL && priv->registration_id > 0) {
    g_dbus_connection_unregister_object(priv->connection, priv->registration_id);
  }

  if (priv->owner_id > 0) {
    g_bus_unown_name(priv->owner_id);
  }

  if (priv->introspection_data != NULL) {
    g_dbus_node_info_unref(priv->introspection_data);
  }

  G_OBJECT_CLASS(zathura_dbus_parent_class)->finalize(object);
}

static void
zathura_dbus_class_init(ZathuraDbusClass* class)
{
  /* overwrite methods */
  GObjectClass* object_class = G_OBJECT_CLASS(class);
  object_class->finalize     = finalize;
}

static void
zathura_dbus_init(ZathuraDbus* dbus)
{
  ZathuraDbusPrivate* priv = zathura_dbus_get_instance_private(dbus);
  priv->zathura            = NULL;
  priv->introspection_data = NULL;
  priv->connection         = NULL;
  priv->owner_id           = 0;
  priv->registration_id    = 0;
}

static void
gdbus_connection_closed(GDBusConnection* UNUSED(connection),
    gboolean UNUSED(remote_peer_vanished), GError* error, void* UNUSED(data))
{
  if (error != NULL) {
    girara_debug("D-Bus connection closed: %s", error->message);
  }
}

static void
bus_acquired(GDBusConnection* connection, const gchar* name, void* data)
{
  girara_debug("Bus acquired at '%s'.", name);

  /* register callback for GDBusConnection's closed signal */
  g_signal_connect(G_OBJECT(connection), "closed",
                   G_CALLBACK(gdbus_connection_closed), NULL);

  ZathuraDbus* dbus        = data;
  ZathuraDbusPrivate* priv = zathura_dbus_get_instance_private(dbus);

  GError* error = NULL;
  priv->registration_id = g_dbus_connection_register_object(
      connection, DBUS_OBJPATH, priv->introspection_data->interfaces[0],
      &interface_vtable, dbus, NULL, &error);
  if (priv->registration_id == 0) {
    girara_warning("Failed to register object on D-Bus connection: %s",
                   error->message);
    g_error_free(error);
    return;
  }

  priv->connection = connection;
}

static void
name_acquired(GDBusConnection* UNUSED(connection), const gchar* name,
              void* UNUSED(data))
{
  girara_debug("Acquired '%s' on session bus.", name);
}

static void
name_lost(GDBusConnection* UNUSED(connection), const gchar* name,
          void* UNUSED(data))
{
  girara_debug("Lost connection or failed to acquire '%s' on session bus.",
               name);
}

ZathuraDbus*
zathura_dbus_new(zathura_t* zathura)
{
  GObject* obj = g_object_new(ZATHURA_TYPE_DBUS, NULL);
  if (obj == NULL) {
    return NULL;
  }

  ZathuraDbus* dbus = ZATHURA_DBUS(obj);
  ZathuraDbusPrivate* priv   = zathura_dbus_get_instance_private(dbus);
  priv->zathura     = zathura;

  GBytes* xml_data = load_xml_data();
  if (xml_data == NULL)
  {
    girara_warning("Failed to load introspection data.");
    g_object_unref(obj);
    return NULL;
  }

  GError* error = NULL;
  priv->introspection_data = g_dbus_node_info_new_for_xml((const char*) g_bytes_get_data(xml_data, NULL), &error);
  g_bytes_unref(xml_data);

  if (priv->introspection_data == NULL) {
    girara_warning("Failed to parse introspection data: %s", error->message);
    g_error_free(error);
    g_object_unref(obj);
    return NULL;
  }

  char* well_known_name = g_strdup_printf(DBUS_NAME_TEMPLATE, getpid());
  priv->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, well_known_name,
                                  G_BUS_NAME_OWNER_FLAGS_NONE, bus_acquired,
                                  name_acquired, name_lost, dbus, NULL);
  g_free(well_known_name);

  return dbus;
}

void
zathura_dbus_edit(ZathuraDbus* edit, unsigned int page, unsigned int x, unsigned int y) {
  ZathuraDbusPrivate* priv = zathura_dbus_get_instance_private(edit);

  const char* filename = zathura_document_get_path(priv->zathura->document);

  char* input_file = NULL;
  unsigned int line = 0;
  unsigned int column = 0;

  if (synctex_get_input_line_column(filename, page, x, y, &input_file, &line,
        &column) == false) {
    return;
  }

  GError* error = NULL;
  g_dbus_connection_emit_signal(priv->connection, NULL, DBUS_OBJPATH,
    DBUS_INTERFACE, "Edit", g_variant_new("(suu)", input_file, line, column), &error);

  g_free(input_file);

  if (error != NULL) {
    girara_debug("Failed to emit 'Edit' signal: %s", error->message);
    g_error_free(error);
  }
}

/* D-Bus handler */

static void
handle_open_document(zathura_t* zathura, GVariant* parameters,
                     GDBusMethodInvocation* invocation)
{
  gchar* filename = NULL;
  gchar* password = NULL;
  gint page = ZATHURA_PAGE_NUMBER_UNSPECIFIED;
  g_variant_get(parameters, "(ssi)", &filename, &password, &page);

  document_close(zathura, false);
  document_open_idle(zathura, filename,
                     strlen(password) > 0 ? password : NULL,
                     page,
                     NULL, NULL, NULL, NULL);
  g_free(filename);
  g_free(password);

  GVariant* result = g_variant_new("(b)", true);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_close_document(zathura_t* zathura, GVariant* UNUSED(parameters),
                      GDBusMethodInvocation* invocation)
{
  const bool ret = document_close(zathura, false);

  GVariant* result = g_variant_new("(b)", ret);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_goto_page(zathura_t* zathura, GVariant* parameters,
                 GDBusMethodInvocation* invocation)
{
  const unsigned int number_of_pages = zathura_document_get_number_of_pages(zathura->document);

  guint page = 0;
  g_variant_get(parameters, "(u)", &page);

  bool ret = true;
  if (page >= number_of_pages) {
    ret = false;
  } else {
    page_set(zathura, page);
  }

  GVariant* result = g_variant_new("(b)", ret);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_highlight_rects(zathura_t* zathura, GVariant* parameters,
                       GDBusMethodInvocation* invocation)
{
  const unsigned int number_of_pages = zathura_document_get_number_of_pages(zathura->document);

  guint page = 0;
  GVariantIter* iter = NULL;
  GVariantIter* secondary_iter = NULL;
  g_variant_get(parameters, "(ua(dddd)a(udddd))", &page, &iter,
                &secondary_iter);

  if (page >= number_of_pages) {
    girara_debug("Got invalid page number.");
    GVariant* result = g_variant_new("(b)", false);
    g_variant_iter_free(iter);
    g_variant_iter_free(secondary_iter);
    g_dbus_method_invocation_return_value(invocation, result);
    return;
  }

  /* get rectangles */
  girara_list_t** rectangles = g_try_malloc0(number_of_pages * sizeof(girara_list_t*));
  if (rectangles == NULL) {
    g_variant_iter_free(iter);
    g_variant_iter_free(secondary_iter);
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                          G_DBUS_ERROR_NO_MEMORY,
                                          "Failed to allocate memory.");
    return;
  }

  rectangles[page] = girara_list_new2(g_free);
  if (rectangles[page] == NULL) {
    g_free(rectangles);
    g_variant_iter_free(iter);
    g_variant_iter_free(secondary_iter);
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                          G_DBUS_ERROR_NO_MEMORY,
                                          "Failed to allocate memory.");
    return;
  }

  zathura_rectangle_t temp_rect = { 0, 0, 0, 0 };
  while (g_variant_iter_loop(iter, "(dddd)", &temp_rect.x1, &temp_rect.x2,
                             &temp_rect.y1, &temp_rect.y2)) {
    zathura_rectangle_t* rect = g_try_malloc0(sizeof(zathura_rectangle_t));
    if (rect == NULL) {
      g_variant_iter_free(iter);
      g_variant_iter_free(secondary_iter);
      girara_list_free(rectangles[page]);
      g_free(rectangles);
      g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                            G_DBUS_ERROR_NO_MEMORY,
                                            "Failed to allocate memory.");
      return;
    }

    *rect = temp_rect;
    girara_list_append(rectangles[page], rect);
  }
  g_variant_iter_free(iter);

  /* get secondary rectangles */
  guint temp_page = 0;
  while (g_variant_iter_loop(secondary_iter, "(udddd)", &temp_page,
                             &temp_rect.x1, &temp_rect.x2, &temp_rect.y1,
                             &temp_rect.y2)) {
    if (temp_page >= number_of_pages) {
      /* error out here? */
      girara_debug("Got invalid page number.");
      continue;
    }

    if (rectangles[temp_page] == NULL) {
      rectangles[temp_page] = girara_list_new2(g_free);
    }

    zathura_rectangle_t* rect = g_try_malloc0(sizeof(zathura_rectangle_t));
    if (rect == NULL || rectangles[temp_page] == NULL) {
      g_variant_iter_free(secondary_iter);
      for (unsigned int p = 0; p != number_of_pages; ++p) {
        girara_list_free(rectangles[p]);
      }
      g_free(rectangles);
      g_free(rect);
      g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                            G_DBUS_ERROR_NO_MEMORY,
                                            "Failed to allocate memory.");
      return;
    }

    *rect = temp_rect;
    girara_list_append(rectangles[temp_page], rect);
  }
  g_variant_iter_free(secondary_iter);

  synctex_highlight_rects(zathura, page, rectangles);
  g_free(rectangles);

  GVariant* result = g_variant_new("(b)", true);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_synctex_view(zathura_t* zathura, GVariant* parameters,
                    GDBusMethodInvocation* invocation)
{
  gchar* input_file = NULL;
  guint line = 0;
  guint column = 0;
  g_variant_get(parameters, "(suu)", &input_file, &line, &column);

  const bool ret = synctex_view(zathura, input_file, line, column);
  g_free(input_file);

  GVariant* result = g_variant_new("(b)", ret);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_execute_command(zathura_t* zathura, GVariant* parameters,
                       GDBusMethodInvocation* invocation)
{
  gchar* input = NULL;
  g_variant_get(parameters, "(s)", &input);

  const bool ret = girara_command_run(zathura->ui.session, input);
  g_free(input);

  GVariant* result = g_variant_new("(b)", ret);
  g_dbus_method_invocation_return_value(invocation, result);
}

static void
handle_method_call(GDBusConnection* UNUSED(connection),
                   const gchar* UNUSED(sender), const gchar* object_path,
                   const gchar* interface_name, const gchar* method_name,
                   GVariant*    parameters, GDBusMethodInvocation* invocation,
                   void* data)
{
  ZathuraDbus* dbus = data;
  ZathuraDbusPrivate* priv   = zathura_dbus_get_instance_private(dbus);

  girara_debug("Handling call '%s.%s' on '%s'.", interface_name, method_name,
               object_path);

  static const struct {
    const char* method;
    void (*handler)(zathura_t*, GVariant*, GDBusMethodInvocation*);
    bool needs_document;
    bool present_window;
  } handlers[] = {
    { "OpenDocument", handle_open_document, false, true },
    { "CloseDocument", handle_close_document, false, false },
    { "GotoPage", handle_goto_page, true, true },
    { "HighlightRects", handle_highlight_rects, true, true },
    { "SynctexView", handle_synctex_view, true, true },
    { "ExecuteCommand", handle_execute_command, false, false }
  };

  for (size_t idx = 0; idx != sizeof(handlers) / sizeof(handlers[0]); ++idx) {
    if (g_strcmp0(method_name, handlers[idx].method) != 0) {
      continue;
    }

    if (handlers[idx].needs_document == true && priv->zathura->document == NULL) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.pwmt.zathura.NoOpenDocument",
          "No document has been opened.");
      return;
    }

    (*handlers[idx].handler)(priv->zathura, parameters, invocation);

    if (handlers[idx].present_window == true && priv->zathura->ui.session->gtk.embed == 0) {
      bool present_window = true;
      girara_setting_get(priv->zathura->ui.session, "dbus-raise-window", &present_window);
      if (present_window == true) {
        gtk_window_present(GTK_WINDOW(priv->zathura->ui.session->gtk.window));
      }
    }

    return;
  }
}

static GVariant*
handle_get_property(GDBusConnection* UNUSED(connection),
                    const gchar* UNUSED(sender),
                    const gchar* UNUSED(object_path),
                    const gchar* UNUSED(interface_name),
                    const gchar* property_name, GError** error, void* data)
{
  ZathuraDbus* dbus        = data;
  ZathuraDbusPrivate* priv = zathura_dbus_get_instance_private(dbus);

  if (priv->zathura->document == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No document open.");
    return NULL;
  }

  if (g_strcmp0(property_name, "filename") == 0) {
    return g_variant_new_string(zathura_document_get_path(priv->zathura->document));
  } else if (g_strcmp0(property_name, "pagenumber") == 0) {
    return g_variant_new_uint32(zathura_document_get_current_page_number(priv->zathura->document));
  } else if (g_strcmp0(property_name, "numberofpages") == 0) {
    return g_variant_new_uint32(zathura_document_get_number_of_pages(priv->zathura->document));
  }

  return NULL;
}

static const GDBusInterfaceVTable interface_vtable =
{
  .method_call  = handle_method_call,
  .get_property = handle_get_property,
  .set_property = NULL
};

static const unsigned int TIMEOUT = 3000;

static bool
call_synctex_view(GDBusConnection* connection, const char* filename,
                  const char* name, const char* input_file, unsigned int line,
                  unsigned int column)
{
  GError* error       = NULL;
  GVariant* vfilename = g_dbus_connection_call_sync(
      connection, name, DBUS_OBJPATH, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", DBUS_INTERFACE, "filename"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, TIMEOUT, NULL, &error);
  if (vfilename == NULL) {
    girara_error("Failed to query 'filename' property from '%s': %s",
                  name, error->message);
    g_error_free(error);
    return false;
  }

  GVariant* tmp = NULL;
  g_variant_get(vfilename, "(v)", &tmp);
  gchar* remote_filename = g_variant_dup_string(tmp, NULL);
  girara_debug("Filename from '%s': %s", name, remote_filename);
  g_variant_unref(tmp);
  g_variant_unref(vfilename);

  if (g_strcmp0(filename, remote_filename) != 0) {
    g_free(remote_filename);
    return false;
  }

  g_free(remote_filename);

  GVariant* ret = g_dbus_connection_call_sync(
      connection, name, DBUS_OBJPATH, DBUS_INTERFACE, "SynctexView",
      g_variant_new("(suu)", input_file, line, column),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, TIMEOUT, NULL, &error);
  if (ret == NULL) {
    girara_error("Failed to run SynctexView on '%s': %s", name,
                 error->message);
    g_error_free(error);
    return false;
  }

  g_variant_unref(ret);
  return true;
}

static int
iterate_instances_call_synctex_view(const char* filename,
                                    const char* input_file, unsigned int line,
                                    unsigned int column, pid_t hint)
{
  if (filename == NULL) {
    return -1;
  }

  GError* error = NULL;
  GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL,
                                               &error);
  if (connection == NULL) {
    girara_error("Could not connect to session bus: %s", error->message);
    g_error_free(error);
    return -1;
  }

  if (hint != -1) {
    char* well_known_name = g_strdup_printf(DBUS_NAME_TEMPLATE, hint);
    const bool ret = call_synctex_view(connection, filename, well_known_name,
                                       input_file, line, column);
    g_free(well_known_name);
    return ret ? 1 : -1;
  }

  GVariant* vnames = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
      G_DBUS_CALL_FLAGS_NONE, TIMEOUT, NULL, &error);
  if (vnames == NULL) {
    girara_error("Could not list available names: %s", error->message);
    g_error_free(error);
    g_object_unref(connection);
    return -1;
  }

  GVariantIter* iter = NULL;
  g_variant_get(vnames, "(as)", &iter);

  gchar* name = NULL;
  bool found_one = false;
  while (found_one == false && g_variant_iter_loop(iter, "s", &name) == TRUE) {
    if (g_str_has_prefix(name, "org.pwmt.zathura.PID") == FALSE) {
      continue;
    }
    girara_debug("Found name: %s", name);

    found_one = call_synctex_view(connection, filename, name, input_file, line, column);
  }
  g_variant_iter_free(iter);
  g_variant_unref(vnames);
  g_object_unref(connection);

  return found_one ? 1 : 0;
}

int
zathura_dbus_synctex_position(const char* filename, const char* input_file,
                              int line, int column, pid_t hint)
{
  if (filename == NULL || input_file == NULL || line < 0 || column < 0) {
    return false;
  }

  return iterate_instances_call_synctex_view(filename, input_file, line, column, hint);
}
