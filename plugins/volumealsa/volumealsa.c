/**
 * Copyright (c) 2008-2014 LxDE Developers, see the file AUTHORS for details.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <libfm/fm-gtk.h>

#include <gst/gst.h>
#include <gst/audio/mixerutils.h>
#include <gst/interfaces/mixer.h>

#include "plugin.h"

#define ICONS_TICK          PACKAGE_DATA_DIR "/images/dialog-ok-apply.png"

#define ICON_BUTTON_TRIM 4

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) g_message("va: " fmt,##args)
#else
#define DEBUG
#endif

typedef enum {
    DEV_HID,
    DEV_AUDIO_SINK,
    DEV_OTHER
} DEVICE_TYPE;

typedef struct {

    /* Graphics. */
    GtkWidget * plugin;             /* Back pointer to the widget */
    LXPanel * panel;                /* Back pointer to panel */
    config_setting_t * settings;        /* Plugin settings */
    GtkWidget * tray_icon;          /* Displayed image */
    GtkWidget * popup_window;           /* Top level window for popup */
    GtkWidget * volume_scale;           /* Scale for volume */
    GtkWidget * mute_check;         /* Checkbox for mute state */
    GtkWidget * menu_popup;
    gboolean show_popup;            /* Toggle to show and hide the popup on left click */
    guint volume_scale_handler;         /* Handler for vscale widget */
    guint mute_check_handler;           /* Handler for mute_check widget */

    /* ALSA interface. */
    snd_mixer_t * mixer;            /* The mixer */
    snd_mixer_elem_t * master_element;      /* The Master element */
    guint mixer_evt_idle;           /* Timer to handle restarting poll */
    guint restart_idle;
    gboolean stopped;

    /* unloading and error handling */
    GIOChannel **channels;                      /* Channels that we listen to */
    guint *watches;                             /* Watcher IDs for channels */
    guint num_channels;                         /* Number of channels */

    /* Icons */
    const char* icon;

    GDBusObjectManager *objmanager;         /* BlueZ object manager */
    char *bt_conname;                       /* BlueZ name of device - just used during connection */
    GDBusConnection *con;                   /* DBus P2P connection to PulseAudio server */
    guint sub_id;                           /* subscription ID for signal notifications on PA server */
    int sink;                               /* sink number of current Bluetooth device - -1 if no device*/
    GtkWidget *conn_dialog, *conn_label, *conn_ok;
} VolumeALSAPlugin;

static void send_message (void);
static gboolean asound_restart(gpointer vol_gpointer);
static gboolean asound_initialize(VolumeALSAPlugin * vol);
static void asound_deinitialize(VolumeALSAPlugin * vol);
static void volumealsa_update_display(VolumeALSAPlugin * vol);
static void volumealsa_destructor(gpointer user_data);
static void volumealsa_build_popup_window(GtkWidget *p);
static guint xfce_mixer_is_default_card (GstElement *card);
static const gchar *xfce_mixer_get_card_display_name (GstElement *card);
static const gchar *xfce_mixer_get_card_id (GstElement *card);
static gboolean xfce_mixer_is_bcm_device (GstElement *card);
static gboolean xfce_mixer_get_bcm_device_id (gchar *id);
static GtkWidget *volumealsa_configure(LXPanel *panel, GtkWidget *p);
static gboolean _xfce_mixer_filter_mixer (GstMixer *mixer, gpointer user_data);
static void _xfce_mixer_destroy_mixer (GstMixer *mixer);


static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static guint32 get_bt_vol (VolumeALSAPlugin *vol);
static gboolean get_bt_mute (VolumeALSAPlugin *vol);
static void set_bt_vol (VolumeALSAPlugin *vol, guint32 volume);
static void set_bt_mute (VolumeALSAPlugin *vol, gboolean mute);
static gboolean get_bz_name_of_pa_sink (VolumeALSAPlugin *vol, char **dev);
static int set_pa_sink_from_bz_name (VolumeALSAPlugin *vol, const char *dev);
static void start_pulseaudio (VolumeALSAPlugin *vol, gboolean always);
static void stop_pulseaudio (VolumeALSAPlugin *vol);
static gboolean check_pulseaudio (void);
static void connect_device (VolumeALSAPlugin *vol);
static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static void disconnect_device (VolumeALSAPlugin *vol);
static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void cb_signal (GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
    const gchar *signal_name, GVariant *parameters, gpointer user_data);
static void set_bt_card_event (GtkWidget * widget, VolumeALSAPlugin * vol);
static void show_connect_dialog (VolumeALSAPlugin *vol, gboolean failed, const gchar *param);
static void handle_close_connect_dialog (GtkButton *button, gpointer user_data);
static gint delete_conn (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void configure_pa (void);
static DEVICE_TYPE check_uuids (VolumeALSAPlugin *vol, const gchar *path);

/* Bluetooth via PulseAudio */

static int get_status (char *cmd)
{
    FILE *fp = popen (cmd, "r");
    char buf[64];
    int res;

    if (fp && fgets (buf, sizeof (buf) - 1, fp) != NULL)
    {
        pclose (fp);
        sscanf (buf, "%d", &res);
        return res;
    }
    if (fp) pclose (fp);
    return 0;
}

static void cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    char *config_file;
    FILE *fp;
    DEBUG ("Name %s owned on DBus", name);

    /* BlueZ exists - get an object manager for it */
    GError *error = NULL;
    vol->objmanager = g_dbus_object_manager_client_new_for_bus_sync (G_BUS_TYPE_SYSTEM, 0, "org.bluez", "/", NULL, NULL, NULL, NULL, &error);
    if (error)
    {
        DEBUG ("Error getting object manager - %s", error->message);
        vol->objmanager = NULL;
        g_error_free (error);
    }

    /* Check whether a Bluetooth audio device is the current default - start PulseAudio if it is */
    config_file = g_build_filename (g_get_home_dir (), ".config", "bt", NULL);
    if (fp = fopen (config_file, "rb"))
    {
        start_pulseaudio (vol, TRUE);
        if (vol->con)
        {
            /* Reconnect the current Bluetooth audio device */
            vol->bt_conname = g_malloc0 (128);
            fgets (vol->bt_conname, 127, fp);
            *(vol->bt_conname + strlen (vol->bt_conname) - 1) = 0;

            DEBUG ("Connecting to %s...", vol->bt_conname);
            connect_device (vol);
        }
        else
        {
            DEBUG ("No connection to PulseAudio");
            stop_pulseaudio (vol);
        }

        fclose (fp);
    }
    else stop_pulseaudio (vol);
}

static void cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);

    if (vol->objmanager) g_object_unref (vol->objmanager);
    vol->objmanager = NULL;

    // kill PulseAudio - it's probably dead anyway...
    stop_pulseaudio (vol);
}

static guint32 get_bt_vol (VolumeALSAPlugin *vol)
{
    GError *error = NULL;
    char buffer[64];
    guint32 res;
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Volume"), NULL, 0, -1, NULL, &error);
    if (error || !var)
    {
        DEBUG ("Cannot get volume - %s", error ? error->message : "no variant");
        if (var) g_variant_unref (var);
        if (error) g_error_free (error);
        return 0;
    }
    res = g_variant_get_uint32 (g_variant_get_child_value (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0), 0));
    g_variant_unref (var);
    return res;
}

static gboolean get_bt_mute (VolumeALSAPlugin *vol)
{
    GError *error = NULL;
    char buffer[64];
    gboolean res;
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    GVariant *var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Mute"), NULL, 0, -1, NULL, &error);
    if (error || !var)
    {
        DEBUG ("Cannot get mute - %s", error ? error->message : "no variant");
        if (var) g_variant_unref (var);
        if (error) g_error_free (error);
        return FALSE;
    }
    res = g_variant_get_boolean (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0));
    g_variant_unref (var);
    return res;
}

static void set_bt_vol (VolumeALSAPlugin *vol, guint32 volume)
{
    GError *error = NULL;
    GVariantBuilder *builder;
    GVariant *var, *res;
    char buffer[64];

    builder = g_variant_builder_new (G_VARIANT_TYPE ("au"));
    g_variant_builder_add (builder, "u", volume);
    var = g_variant_new ("(ssv)", "org.PulseAudio.Core1.Device", "Volume", g_variant_new ("au", builder));
    g_variant_ref_sink (var);
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    res = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Set", var, NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Cannot set volume - %s", error->message);
        g_error_free (error);
    }
    g_variant_unref (res);
    g_variant_unref (var);
    g_variant_builder_unref (builder);
}

static void set_bt_mute (VolumeALSAPlugin *vol, gboolean mute)
{
    GError *error = NULL;
    GVariant *var, *res;
    char buffer[64];

    var = g_variant_new ("(ssv)", "org.PulseAudio.Core1.Device", "Mute", g_variant_new ("b", mute));
    g_variant_ref_sink (var);
    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    res = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Set", var, NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Cannot set mute - %s", error->message);
        g_error_free (error);
    }
    g_variant_unref (res);
    g_variant_unref (var);
}

static gboolean get_bz_name_of_pa_sink (VolumeALSAPlugin *vol, char **dev)
{
    GError *error;
    GVariant *var1, *var2;
    const gchar *key;

    // query PulseAudio for the path of the default sink - returned in the format "/org/pulseaudio/core1/sinkn"
    error = NULL;
    var1 = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1", "FallbackSink"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Cannot get path for default device - %s", error->message);
        if (var1) g_variant_unref (var1);
        g_error_free (error);
        return FALSE;
    }
    if (!var1)
    {
        DEBUG ("Default device not set");
        return FALSE;
    }

    // query the device with that path for its name - returned in the format "bluez_sink.aa_bb_cc_dd_ee_ff"
    key = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var1, 0), 0), NULL);
    error = NULL;
    var2 = g_dbus_connection_call_sync (vol->con, NULL, key, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Cannot get name of default device - %s", error->message);
        g_variant_unref (var1);
        if (var2) g_variant_unref (var2);
        g_error_free (error);
        return FALSE;
    }
    if (!var2)
    {
        DEBUG ("Default device has null name");
        g_variant_unref (var1);
        return FALSE;
    }
    *dev = g_variant_dup_string (g_variant_get_child_value (g_variant_get_child_value (var2, 0), 0), NULL);
    g_variant_unref (var1);
    g_variant_unref (var2);
    return TRUE;
}

static int set_pa_sink_from_bz_name (VolumeALSAPlugin *vol, const char *dev)
{
    GError *error;
    GVariant *var1, *var2, *res;
    GVariantIter iter;
    gchar *key;
    const gchar *dev2;
    int sink;

    // get the list of PulseAudio sinks - returned as an array of paths in the format "/org/pulseaudio/core1/sinkn"
    error = NULL;
    var1 = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1", "Sinks"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Could not read sink list - %s", error->message);
        if (var1) g_variant_unref (var1);
        g_error_free (error);
        return -1;
    }
    if (!var1)
    {
        DEBUG ("Sink list null");
        return -1;
    }

    // iterate through the sink list
    g_variant_iter_init (&iter, g_variant_get_child_value (g_variant_get_child_value (var1, 0), 0));
    while (g_variant_iter_next (&iter, "o", &key, NULL))
    {
        // for each sink in the list, query the device with that path for its name - returned in the format "bluez_sink.aa_bb_cc_dd_ee_ff"
        error = NULL;
        var2 = g_dbus_connection_call_sync (vol->con, NULL, key, "org.freedesktop.DBus.Properties", "Get", g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
        if (error)
        {
            DEBUG ("Cannot get name of device - %s", error->message);
            g_variant_unref (var1);
            if (var2) g_variant_unref (var2);
            g_error_free (error);
            return -1;
        }
        if (!var2)
        {
            DEBUG ("Default device has null name");
            g_variant_unref (var1);
            return -1;
        }
        dev2 = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var2, 0), 0), NULL);

        // if the six octets at the end of the name match those supplied, set as the default sink
        if (!g_strcmp0 (dev + 20, dev2 + 11))
        {
            if (sscanf (key, "/org/pulseaudio/core1/sink%d", &sink) != 1) return -1;

            error = NULL;
            res = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.freedesktop.DBus.Properties", "Set",
                g_variant_new ("(ssv)", "org.PulseAudio.Core1", "FallbackSink", g_variant_new ("o", key)), NULL, 0, -1, NULL, &error);
            if (error)
            {
                DEBUG ("Cannot set sink - %s", error->message);
                g_error_free (error);
            }
            if (res) g_variant_unref (res);
            return sink;
        }
    }
    g_variant_unref (var1);
    if (var2) g_variant_unref (var2);
    return -1;
}

static void start_pulseaudio (VolumeALSAPlugin *vol, gboolean always)
{
    GDBusConnection *con;
    GDBusProxy *prox;
    GVariant *var;
    GError *error;

    DEBUG ("Starting PulseAudio");

    // fall out if PA already started...
    if (!always && check_pulseaudio ()) return;

    vol->con = NULL;

    // start the PulseAudio server if it isn't running
    system ("pulseaudio -D --load=module-dbus-protocol");

    // request the PulseAudio P2P address from the PulseAudio server
    error = NULL;
    con = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    if (error)
    {
        DEBUG ("Bus connection error - %s", error->message);
        g_error_free (error);
        return;
    }

    error = NULL;
    prox = g_dbus_proxy_new_sync (con, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.PulseAudio1", "/org/pulseaudio/server_lookup1", "org.freedesktop.DBus.Properties", NULL, NULL);
    if (error)
    {
        DEBUG ("Error requesting address - %s", error->message);
        g_error_free (error);
        return;
    }
    g_object_unref (con);

    var = g_dbus_proxy_get_cached_property (prox, "Address");
    if (!var)
    {
        DEBUG ("Null address");
        return;
    }
    g_object_unref (prox);

    // create a P2P connection to PulseAudio at the returned address
    error = NULL;
    vol->con = g_dbus_connection_new_for_address_sync (g_variant_get_string (var, NULL), G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
    if (error)
    {
        DEBUG ("Connection error - %s", error->message);
        vol->con = NULL;
        g_error_free (error);
        return;
    }
    g_variant_unref (var);

    // request SinkRemoved signals from PulseAudio
    error = NULL;
    var = g_dbus_connection_call_sync (vol->con, NULL, "/org/pulseaudio/core1", "org.PulseAudio.Core1", "ListenForSignal", g_variant_new ("(sao)", "org.PulseAudio.Core1.SinkRemoved", NULL), NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Method error - %s", error->message);
        g_error_free (error);
    }
    g_variant_unref (var);

    // set up callback on SinkRemoved signal
    error = NULL;
    vol->sub_id = g_dbus_connection_signal_subscribe (vol->con, NULL, "org.PulseAudio.Core1", NULL, NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, cb_signal, vol, NULL);
    if (error)
    {
        DEBUG ("Subscribe error - %s", error->message);
        g_error_free (error);
    }
}

static void stop_pulseaudio (VolumeALSAPlugin *vol)
{
    DEBUG ("Stopping PulseAudio");
    if (!check_pulseaudio ()) return;
    
    if (vol->sub_id) g_dbus_connection_signal_unsubscribe (vol->con, vol->sub_id);
    vol->sub_id = 0;
    system ("pulseaudio --kill");
    system ("rm ~/.config/bt");
    g_object_unref (vol->con);
    vol->con = NULL;
    vol->sink = -1;
}

static gboolean check_pulseaudio (void)
{
    return get_status ("! pulseaudio --check ; echo $?");
}

static void connect_device (VolumeALSAPlugin *vol)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
    if (interface)
    {
        // trust and connect
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set", g_variant_new ("(ssv)",
            g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", g_variant_new_boolean (TRUE)), G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_trusted, vol);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_connected, vol);
        g_object_unref (interface);
    }
    else
    {
        DEBUG ("Couldn't get device interface from object manager");
        stop_pulseaudio (vol);
        if (vol->conn_dialog) show_connect_dialog (vol, TRUE, _("Could not get BlueZ interface"));
    }
}

static void cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    char buffer[256];
    GtkWidget *dlg, *lbl, *btn;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // kill PulseAudio and fall back to an internal device
        stop_pulseaudio (vol);

        // update dialog to show a warning
        if (vol->conn_dialog) show_connect_dialog (vol, TRUE, error->message);
    }
    else
    {
        DEBUG ("Connected OK");

        // save the connection data to a file for use after reboot
        sprintf (buffer, "echo %s > ~/.config/bt", vol->bt_conname);
        system (buffer);

        // update the globals with connection details
        vol->sink = set_pa_sink_from_bz_name (vol, vol->bt_conname);

        // close the connection dialog
        handle_close_connect_dialog (NULL, vol);
    }

    // delete the connection information
    g_free (vol->bt_conname);
    vol->bt_conname = NULL;

    // update the display whether we succeeded or not, as a failure will have caused a fallback to ALSA
    volumealsa_update_display (vol);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static void cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Trusting error %s", error->message);
        g_error_free (error);
    }
    else DEBUG ("Trusted OK");
}

static void disconnect_device (VolumeALSAPlugin *vol)
{
    // get the name of the device with the current sink number
    GError *error = NULL;
    GVariant *var;
    char buffer[64];

    sprintf (buffer, "/org/pulseaudio/core1/sink%d", vol->sink);
    vol->sink = -1;

    var = g_dbus_connection_call_sync (vol->con, NULL, buffer, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new ("(ss)", "org.PulseAudio.Core1.Device", "Name"), NULL, 0, -1, NULL, &error);
    if (error)
    {
        DEBUG ("Could not read device name to disconnect - %s", error->message);
        if (var) g_variant_unref (var);
        g_error_free (error);
    }
    else if (!var)
    {
        DEBUG ("Device name to disconnect null");
    }
    else
    {
        DEBUG ("Device to disconnect = %s", g_variant_print (var, TRUE));
        // convert into a BlueZ device path
        const gchar *res = g_variant_get_string (g_variant_get_child_value (g_variant_get_child_value (var, 0), 0), NULL);
        sprintf (buffer, "/org/bluez/hci0/dev_%s", res + 11);

        // call the disconnect method on BlueZ
        GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, buffer, "org.bluez.Device1");
        if (interface)
        {
            DEBUG ("Disconnecting...");
            g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, cb_disconnected, vol);
            g_variant_unref (var);
            g_object_unref (interface);
            return;
        }
        g_variant_unref (var);
    }

    // if no connection found, just make the new connection
    DEBUG ("Nothing to disconnect - connecting to %s...", vol->bt_conname);
    connect_device (vol);
}

static void cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;
    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Disconnect error %s", error->message);
        g_error_free (error);
    }
    else DEBUG ("Disconnected OK");

    // call BlueZ over DBus to connect to the device
    DEBUG ("Connecting to %s...", vol->bt_conname);
    connect_device (vol);
}

static void cb_signal (GDBusConnection *connection, const gchar *sender_name, const gchar *object_path, const gchar *interface_name,
    const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Signal : %s %s %s %s %s", sender_name, object_path, interface_name, signal_name, g_variant_print (parameters, TRUE));
    if (!g_strcmp0 (signal_name, "SinkRemoved"))
    {
        int sink;
        const char *sinkname = g_variant_get_string (g_variant_get_child_value (parameters, 0), NULL);
        if (sscanf (sinkname, "/org/pulseaudio/core1/sink%d", &sink) == 1 && sink == vol->sink)
        {
            DEBUG ("Sink in use removed");
            stop_pulseaudio (vol);
            volumealsa_update_display (vol);
        }
    }
}

static void set_bt_card_event (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    start_pulseaudio (vol, FALSE);

    // show the connection dialog
    show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

    if (vol->con)
    {
        // store the name of the BlueZ device to connect to for use in the callback
        vol->bt_conname = g_malloc0 (strlen (widget->name) + 1);
        strcpy (vol->bt_conname, widget->name);

        if (vol->sink != -1)
        {
            // already connected to a Bluetooth device - disconnect it
            DEBUG ("Disconnecting existing Bluetooth audio device");
            disconnect_device (vol);
        }
        else
        {
            // call BlueZ over DBus to connect to the device
            DEBUG ("Connecting to %s...", vol->bt_conname);
            connect_device (vol);
        }
    }
    else
    {
        DEBUG ("No connection to PulseAudio");
        stop_pulseaudio (vol);
        if (vol->conn_dialog) show_connect_dialog (vol, TRUE, _("Could not connect to PulseAudio"));
    }
}

static void show_connect_dialog (VolumeALSAPlugin *vol, gboolean failed, const gchar *param)
{
    char buffer[256], path[128];

    if (!failed)
    {
        vol->conn_dialog = gtk_dialog_new_with_buttons (_("Connecting Audio Device"), NULL, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
        sprintf (path, "%s/images/preferences-system-bluetooth.png", PACKAGE_DATA_DIR);
        gtk_window_set_icon (GTK_WINDOW (vol->conn_dialog), gdk_pixbuf_new_from_file (path, NULL));
        gtk_window_set_position (GTK_WINDOW (vol->conn_dialog), GTK_WIN_POS_CENTER);
        gtk_container_set_border_width (GTK_CONTAINER (vol->conn_dialog), 10);
        sprintf (buffer, _("Connecting to Bluetooth audio device '%s'..."), param);
        vol->conn_label = gtk_label_new (buffer);
        gtk_label_set_line_wrap (GTK_LABEL (vol->conn_label), TRUE);
        gtk_label_set_justify (GTK_LABEL (vol->conn_label), GTK_JUSTIFY_LEFT);
        gtk_misc_set_alignment (GTK_MISC (vol->conn_label), 0.0, 0.0);
        gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (vol->conn_dialog))), vol->conn_label, TRUE, TRUE, 0);
        g_signal_connect (GTK_OBJECT (vol->conn_dialog), "delete_event", G_CALLBACK (delete_conn), vol);
        gtk_widget_show_all (vol->conn_dialog);
    }
    else
    {
        sprintf (buffer, _("Failed to connect to device - %s. Try to connect again."), param);
        gtk_label_set_text (GTK_LABEL (vol->conn_label), buffer);
        vol->conn_ok = gtk_dialog_add_button (GTK_DIALOG (vol->conn_dialog), _("_OK"), 1);
        g_signal_connect (vol->conn_ok, "clicked", G_CALLBACK (handle_close_connect_dialog), vol);
        gtk_widget_show (vol->conn_ok);
    }
}

static void handle_close_connect_dialog (GtkButton *button, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (vol->conn_dialog)
    {
        gtk_widget_destroy (vol->conn_dialog);
        vol->conn_dialog = NULL;
    }
}

static gint delete_conn (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (vol->conn_dialog)
    {
        gtk_widget_destroy (vol->conn_dialog);
        vol->conn_dialog = NULL;
    }
    return TRUE;
}

static void configure_pa (void)
{
    // check the local configuration file
    FILE *fp;
    char *config_file;
    config_file = g_build_filename (g_get_home_dir (), ".config", "pulse", "client.conf", NULL);
    if (fp = fopen (config_file, "rb"))
    {
        fclose (fp);

        // check and update relevant strings in file
        if (get_status ("! grep -E -q \"autospawn\" ~/.config/pulse/client.conf ; echo $?"))
        {
            system ("sed -i 's/.*autospawn.*/autospawn = no/g' ~/.config/pulse/client.conf");
        }
        else system ("echo \"autospawn = no\n\" >> ~/.config/pulse/client.conf");
        if (get_status ("! grep -E -q \"daemon-binary\" ~/.config/pulse/client.conf ; echo $?"))
        {
            system ("sed -i 's|.*daemon-binary.*|daemon-binary = /bin/true|g' ~/.config/pulse/client.conf");
        }
        else system ("echo \"daemon-binary = /bin/true\n\" >> ~/.config/pulse/client.conf");
    }
    else
    {
        // create file with default contents
        system ("echo \"autospawn = no\ndaemon-binary = /bin/true\n\" > ~/.config/pulse/client.conf");
    }
    g_free (config_file);
}

static DEVICE_TYPE check_uuids (VolumeALSAPlugin *vol, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, path, "org.bluez.Device1");
    GVariant *elem, *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "UUIDs");
    GVariantIter iter;
    g_variant_iter_init (&iter, var);
    while (elem = g_variant_iter_next_value (&iter))
    {
        const char *uuid = g_variant_get_string (elem, NULL);
        if (!strncasecmp (uuid, "00001124", 8)) return DEV_HID;
        if (!strncasecmp (uuid, "0000110B", 8)) return DEV_AUDIO_SINK;
        g_variant_unref (elem);
    }
    g_variant_unref (var);
    g_object_unref (interface);
    return DEV_OTHER;
}

/*** ALSA ***/

static gboolean asound_find_elements(VolumeALSAPlugin * vol)
{
    const char *name;
    for (
      vol->master_element = snd_mixer_first_elem(vol->mixer);
      vol->master_element != NULL;
      vol->master_element = snd_mixer_elem_next(vol->master_element))
    {
        if ((snd_mixer_selem_is_active(vol->master_element)))
        {
            name = snd_mixer_selem_get_name(vol->master_element);
            if (!strncmp (name, "Master", 6)) return TRUE;
            if (!strncmp (name, "Front", 5)) return TRUE;
            if (!strncmp (name, "PCM", 3)) return TRUE;
            if (!strncmp (name, "LineOut", 7)) return TRUE;
            if (!strncmp (name, "Digital", 7)) return TRUE;
            if (!strncmp (name, "Headphone", 9)) return TRUE;
            if (!strncmp (name, "Speaker", 7)) return TRUE;
        }
    }
    return FALSE;
}

/* NOTE by PCMan:
 * This is magic! Since ALSA uses its own machanism to handle this part.
 * After polling of mixer fds, it requires that we should call
 * snd_mixer_handle_events to clear all pending mixer events.
 * However, when using the glib IO channels approach, we don't have
 * poll() and snd_mixer_poll_descriptors_revents(). Due to the design of
 * glib, on_mixer_event() will be called for every fd whose status was
 * changed. So, after each poll(), it's called for several times,
 * not just once. Therefore, we cannot call snd_mixer_handle_events()
 * directly in the event handler. Otherwise, it will get called for
 * several times, which might clear unprocessed pending events in the queue.
 * So, here we call it once in the event callback for the first fd.
 * Then, we don't call it for the following fds. After all fds with changed
 * status are handled, we remove this restriction in an idle handler.
 * The next time the event callback is involked for the first fs, we can
 * call snd_mixer_handle_events() again. Racing shouldn't happen here
 * because the idle handler has the same priority as the io channel callback.
 * So, io callbacks for future pending events should be in the next gmain
 * iteration, and won't be affected.
 */

static gboolean asound_reset_mixer_evt_idle(VolumeALSAPlugin * vol)
{
    if (!g_source_is_destroyed(g_main_current_source()))
        vol->mixer_evt_idle = 0;
    return FALSE;
}

/* Handler for I/O event on ALSA channel. */
static gboolean asound_mixer_event(GIOChannel * channel, GIOCondition cond, gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = (VolumeALSAPlugin *) vol_gpointer;
    int res = 0;

    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    if (vol->mixer_evt_idle == 0)
    {
        vol->mixer_evt_idle = g_idle_add_full(G_PRIORITY_DEFAULT, (GSourceFunc) asound_reset_mixer_evt_idle, vol, NULL);
        res = snd_mixer_handle_events(vol->mixer);
    }

    if (cond & G_IO_IN)
    {
        /* the status of mixer is changed. update of display is needed. */
        volumealsa_update_display(vol);
    }

    if ((cond & G_IO_HUP) || (res < 0))
    {
        /* This means there're some problems with alsa. */
        g_warning("volumealsa: ALSA (or pulseaudio) had a problem: "
                "volumealsa: snd_mixer_handle_events() = %d,"
                " cond 0x%x (IN: 0x%x, HUP: 0x%x).", res, cond,
                G_IO_IN, G_IO_HUP);
        gtk_widget_set_tooltip_text(vol->plugin, "ALSA (or pulseaudio) had a problem."
                " Please check the lxpanel logs.");

        if (vol->restart_idle == 0)
            vol->restart_idle = g_timeout_add_seconds(1, asound_restart, vol);

        return FALSE;
    }

    return TRUE;
}

static gboolean asound_restart(gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = vol_gpointer;

    if (!g_main_current_source()) return TRUE;
    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    asound_deinitialize(vol);

    if (!asound_initialize(vol)) {
        g_warning("volumealsa: Re-initialization failed.");
        return TRUE; // try again in a second
    }

    g_warning("volumealsa: Restarted ALSA interface...");

    vol->restart_idle = 0;
    return FALSE;
}

static void asound_get_default_card (char *id)
{
  char tokenbuf[256], type[16], cid[16], state = 0, indef = 0;
  char *bufptr = tokenbuf;
  int inchar, count;
  char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
  FILE *fp = fopen (user_config_file, "rb");
  type[0] = 0;
  cid[0] = 0;
  if (fp)
  {
    count = 0;
    while ((inchar = fgetc (fp)) != EOF)
    {
        if (inchar == ' ' || inchar == '\t' || inchar == '\n' || inchar == '\r')
        {
            if (bufptr != tokenbuf)
            {
                *bufptr = 0;
                switch (state)
                {
                    case 1 :    strcpy (type, tokenbuf);
                                state = 0;
                                break;
                    case 2 :    strcpy (cid, tokenbuf);
                                state = 0;
                                break;
                    default :   if (!strcmp (tokenbuf, "type") && indef) state = 1;
                                else if (!strcmp (tokenbuf, "card") && indef) state = 2;
                                else if (!strcmp (tokenbuf, "pcm.!default")) indef = 1;
                                else if (!strcmp (tokenbuf, "}")) indef = 0;
                                break;
                }
                bufptr = tokenbuf;
                count = 0;
                if (cid[0] && type[0]) break;
            }
            else
            {
                bufptr = tokenbuf;
                count = 0;
            }
        }
        else
        {
            if (count < 255)
            {
                *bufptr++ = inchar;
                count++;
            }
            else tokenbuf[255] = 0;
        }
    }
    fclose (fp);
  }
  if (cid[0] && type[0]) sprintf (id, "%s:%s", type, cid);
  else sprintf (id, "hw:0");
  g_free (user_config_file);
}

static void asound_set_default_card (const char *id)
{
  char cmdbuf[256], idbuf[16], type[16], cid[16], *card, *bufptr = cmdbuf, state = 0, indef = 0;
  int inchar, count;
  char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

  // Break the id string into the type (before the colon) and the card number (after the colon)
  strcpy (idbuf, id);
  card = strchr (idbuf, ':') + 1;
  *(strchr (idbuf, ':')) = 0;

  FILE *fp = fopen (user_config_file, "rb");
  if (!fp)
  {
    // File does not exist - create it from scratch
    fp = fopen (user_config_file, "wb");
    fprintf (fp, "pcm.!default {\n\ttype %s\n\tcard %s\n}\n\nctl.!default {\n\ttype %s\n\tcard %s\n}\n", idbuf, card, idbuf, card);
    fclose (fp);
  }
  else
  {
    // File exists - check to see whether it contains a default card
    type[0] = 0;
    cid[0] = 0;
    count = 0;
    while ((inchar = fgetc (fp)) != EOF)
    {
        if (inchar == ' ' || inchar == '\t' || inchar == '\n' || inchar == '\r')
        {
            if (bufptr != cmdbuf)
            {
                *bufptr = 0;
                switch (state)
                {
                    case 1 :    strcpy (type, cmdbuf);
                                state = 0;
                                break;
                    case 2 :    strcpy (cid, cmdbuf);
                                state = 0;
                                break;
                    default :   if (!strcmp (cmdbuf, "type") && indef) state = 1;
                                else if (!strcmp (cmdbuf, "card") && indef) state = 2;
                                else if (!strcmp (cmdbuf, "pcm.!default")) indef = 1;
                                else if (!strcmp (cmdbuf, "}")) indef = 0;
                                break;
                }
                bufptr = cmdbuf;
                count = 0;
                if (cid[0] && type[0]) break;
            }
            else
            {
                bufptr = cmdbuf;
                count = 0;
            }
        }
        else
        {
            if (count < 255)
            {
                *bufptr++ = inchar;
                count++;
            }
            else cmdbuf[255] = 0;
        }
    }
    fclose (fp);
    if (cid[0] && type[0])
    {
        // This piece of sed is surely self-explanatory...
        sprintf (cmdbuf, "sed -i '/pcm.!default\\|ctl.!default/,/}/ { s/type .*/type %s/g; s/card .*/card %s/g; }' %s", idbuf, card, user_config_file);
        system (cmdbuf);
        // Oh, OK then - it looks for type * and card * within the delimiters pcm.!default or ctl.!default and } and replaces the parameters
    }
    else
    {
        // No default card; append to end of file
        fp = fopen (user_config_file, "ab");
        fprintf (fp, "\n\npcm.!default {\n\ttype %s\n\tcard %s\n}\n\nctl.!default {\n\ttype %s\n\tcard %s\n}\n", idbuf, card, idbuf, card);
        fclose (fp);
    }
  }
  g_free (user_config_file);
}

static gboolean asound_set_bcm_card (void)
{
    char bcmdev[64];
    if (xfce_mixer_get_bcm_device_id (bcmdev))
    {
        asound_set_default_card (bcmdev);
        return TRUE;
    }
    return FALSE;
}

static int asound_get_bcm_output (void)
{
    int val = -1, tmp;
    char buf[128];
    FILE *res = popen ("amixer cget numid=3", "r");
    while (!feof (res))
    {
        fgets (buf, 128, res);
        if (sscanf (buf, "  : values=%d", &tmp)) val = tmp;
    }
    pclose (res);

    if (val == 0)
    {
        /* set to analog if result is auto */
        system ("amixer cset numid=3 2");
        val = 2;
    }
    return val;
}

static void asound_find_valid_device (void)
{
    // call this if the current ALSA device is invalid - it tries to find an alternative
    g_warning ("volumealsa: Default ALSA device not valid - resetting to internal");
    if (!asound_set_bcm_card ())
    {
        GList *mixers;
        gint counter = 0;

        g_warning ("volumealsa: Internal device not available - looking for first valid ALSA device...");
        mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);
        if (mixers)
        {
            const char *card = xfce_mixer_get_card_id (mixers->data);
            if (card)
            {
                g_warning ("volumealsa: Valid ALSA device %s found", card);
                asound_set_default_card (card);
                g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
                return;
            }
            g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
        }
        g_warning ("volumealsa: No ALSA devices found");
    }
}

/* Initialize the ALSA interface. */
static gboolean asound_initialize(VolumeALSAPlugin * vol)
{
    char device[32];

    asound_get_default_card (device);
    /* Access the "default" device. */
    snd_mixer_open(&vol->mixer, 0);
    if (snd_mixer_attach(vol->mixer, device))
    {
        g_warning ("volumealsa: Couldn't attach mixer - looking for another valid device");
        asound_find_valid_device ();
        asound_get_default_card (device);
        snd_mixer_attach(vol->mixer, device);
    }
    snd_mixer_selem_register(vol->mixer, NULL, NULL);
    snd_mixer_load(vol->mixer);

    /* Find Master element, or Front element, or PCM element, or LineOut element.
     * If one of these succeeds, master_element is valid. */
    if (!asound_find_elements (vol))
    {
        // this is a belt-and-braces check in case a driver has become corrupt...
        g_warning ("volumealsa: Can't find elements - trying to reset to internal");
        snd_mixer_detach (vol->mixer, device);
        snd_mixer_free (vol->mixer);
        asound_find_valid_device ();
        asound_get_default_card (device);
        snd_mixer_open (&vol->mixer, 0);
        snd_mixer_attach (vol->mixer, device);
        snd_mixer_selem_register (vol->mixer, NULL, NULL);
        snd_mixer_load (vol->mixer);
        if (!asound_find_elements (vol)) return FALSE;
   }

    /* Set the playback volume range as we wish it. */
    snd_mixer_selem_set_playback_volume_range(vol->master_element, 0, 100);

    /* Listen to events from ALSA. */
    int n_fds = snd_mixer_poll_descriptors_count(vol->mixer);
    struct pollfd * fds = g_new0(struct pollfd, n_fds);

    vol->channels = g_new0(GIOChannel *, n_fds);
    vol->watches = g_new0(guint, n_fds);
    vol->num_channels = n_fds;

    snd_mixer_poll_descriptors(vol->mixer, fds, n_fds);
    int i;
    for (i = 0; i < n_fds; ++i)
    {
        GIOChannel* channel = g_io_channel_unix_new(fds[i].fd);
        vol->watches[i] = g_io_add_watch(channel, G_IO_IN | G_IO_HUP, asound_mixer_event, vol);
        vol->channels[i] = channel;
    }
    g_free(fds);
    return TRUE;
}

static void asound_deinitialize(VolumeALSAPlugin * vol)
{
    guint i;

    if (vol->mixer_evt_idle != 0) {
        g_source_remove(vol->mixer_evt_idle);
        vol->mixer_evt_idle = 0;
    }

    for (i = 0; i < vol->num_channels; i++) {
        g_source_remove(vol->watches[i]);
        g_io_channel_shutdown(vol->channels[i], FALSE, NULL);
        g_io_channel_unref(vol->channels[i]);
    }
    g_free(vol->channels);
    g_free(vol->watches);
    vol->channels = NULL;
    vol->watches = NULL;
    vol->num_channels = 0;

    char device[32];
    asound_get_default_card (device);
    if (device) snd_mixer_detach (vol->mixer, device);
    snd_mixer_close(vol->mixer);
    vol->master_element = NULL;
    vol->mixer = NULL;
}

/* Get the presence of the mute control from the sound system. */
static gboolean asound_has_mute(VolumeALSAPlugin * vol)
{
    return ((vol->master_element != NULL) ? snd_mixer_selem_has_playback_switch(vol->master_element) : FALSE);
}

/* Get the condition of the mute control from the sound system. */
static gboolean asound_is_muted(VolumeALSAPlugin * vol)
{
    /* The switch is on if sound is not muted, and off if the sound is muted.
     * Initialize so that the sound appears unmuted if the control does not exist. */
    int value = 1;
    if (vol->master_element != NULL)
        snd_mixer_selem_get_playback_switch(vol->master_element, 0, &value);
    return (value == 0);
}

/* Get the volume from the sound system.
 * This implementation returns the average of the Front Left and Front Right channels. */
static int asound_get_volume(VolumeALSAPlugin * vol)
{
    long aleft = 0;
    long aright = 0;
    if (vol->master_element != NULL)
    {
        snd_mixer_selem_get_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, &aleft);
        snd_mixer_selem_get_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT, &aright);
    }
    return (aleft + aright) >> 1;
}

/* Set the volume to the sound system.
 * This implementation sets the Front Left and Front Right channels to the specified value. */
static void asound_set_volume(VolumeALSAPlugin * vol, int volume)
{
    if (vol->master_element != NULL)
    {
        snd_mixer_selem_set_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, volume);
        snd_mixer_selem_set_playback_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT, volume);
    }
}

/*** Graphics ***/

static void volumealsa_update_current_icon(VolumeALSAPlugin * vol)
{
    /* Mute status. */
    gboolean mute;
    int level;
    /* Mute status. */
    if (vol->sink == -1)
    {
        mute = asound_is_muted(vol);
        level = asound_get_volume(vol);
    }
    else
    {
        mute = get_bt_mute (vol);
        level = get_bt_vol (vol) / 656;
    }

    /* Change icon according to mute / volume */
    const char* icon="audio-volume-muted";
    if (mute)
    {
         icon="audio-volume-muted";
    }
    else if (level >= 75)
    {
         icon="audio-volume-high";
    }
    else if (level >= 50)
    {
         icon="audio-volume-medium";
    }
    else if (level > 0)
    {
         icon="audio-volume-low";
    }

    vol->icon = icon;
}

void set_icon (LXPanel *p, GtkWidget *image, const char *icon, int size)
{
    GdkPixbuf *pixbuf;
    if (size == 0) size = panel_get_icon_size (p) - ICON_BUTTON_TRIM;
    if (gtk_icon_theme_has_icon (panel_get_icon_theme (p), icon))
    {
        GtkIconInfo *info = gtk_icon_theme_lookup_icon (panel_get_icon_theme (p), icon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
        pixbuf = gtk_icon_info_load_icon (info, NULL);
        gtk_icon_info_free (info);
        if (pixbuf != NULL)
        {
            gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
            g_object_unref (pixbuf);
            return;
        }
    }
    else
    {
        char path[256];
        sprintf (path, "%s/images/%s.png", PACKAGE_DATA_DIR, icon);
        pixbuf = gdk_pixbuf_new_from_file_at_scale (path, size, size, TRUE, NULL);
        if (pixbuf != NULL)
        {
            gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
            g_object_unref (pixbuf);
        }
    }
}

/* Do a full redraw of the display. */
static void volumealsa_update_display(VolumeALSAPlugin * vol)
{
    gboolean mute;
    int level;
    /* Mute status. */
    if (vol->sink == -1)
    {
        mute = asound_is_muted(vol);
        level = asound_get_volume(vol);
    }
    else
    {
        mute = get_bt_mute (vol);
        level = get_bt_vol (vol) / 656;
    }
    if (mute) level = 0;

    volumealsa_update_current_icon(vol);

    /* Change icon, fallback to default icon if theme doesn't exsit */
    set_icon (vol->panel, vol->tray_icon, vol->icon, 0);

    g_signal_handler_block(vol->mute_check, vol->mute_check_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(vol->mute_check), mute);
    if (vol->sink == -1)
        gtk_widget_set_sensitive(vol->mute_check, asound_has_mute(vol));
    else
        gtk_widget_set_sensitive(vol->mute_check, TRUE);
    g_signal_handler_unblock(vol->mute_check, vol->mute_check_handler);

    /* Volume. */
    if (vol->volume_scale != NULL)
    {
        g_signal_handler_block(vol->volume_scale, vol->volume_scale_handler);
        gtk_range_set_value(GTK_RANGE(vol->volume_scale), level);
        g_signal_handler_unblock(vol->volume_scale, vol->volume_scale_handler);
    }

    /* Display current level in tooltip. */
    char * tooltip = g_strdup_printf("%s %d", _("Volume control"), level);
    gtk_widget_set_tooltip_text(vol->plugin, tooltip);
    g_free(tooltip);
}

/*** Gstreamer access functions copied from xfce_mixer ***/

static void
_xfce_mixer_destroy_mixer (GstMixer *mixer)
{
  gst_element_set_state (GST_ELEMENT (mixer), GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (mixer));
}

static gboolean
_xfce_mixer_filter_mixer (GstMixer *mixer,
                          gpointer  user_data)
{
  GstElementFactory *factory;
  const gchar       *long_name;
  gchar             *device_name = NULL;
  gchar             *internal_name;
  gchar             *name;
  gchar             *p;
  gint               length;
  gint              *counter = user_data;
  gchar *device;

  /* Get long name of the mixer element */
  factory = gst_element_get_factory (GST_ELEMENT (mixer));
  long_name = gst_element_factory_get_longname (factory);

  /* Get the device name of the mixer element */
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (mixer)), "device-name"))
    g_object_get (mixer, "device-name", &device_name, NULL);

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (mixer)), "device"))
    g_object_get (mixer, "device", &device, NULL);

  /* Fall back to default name if neccessary */
  if (G_UNLIKELY (device_name == NULL))
    device_name = g_strdup_printf (_("Unknown Volume Control %d"), (*counter)++);

  /* Build display name */
  name = g_strdup_printf ("%s (%s)", device_name, long_name);

  /* Free device name */
  g_free (device_name);

  /* Set name to be used by xfce4-mixer */
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-name", name, (GDestroyNotify) g_free);
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-id", device, (GDestroyNotify) g_free);

  /* Count alpha-numeric characters in the name */
  for (length = 0, p = name; *p != '\0'; ++p)
    if (g_ascii_isalnum (*p))
      ++length;

  /* Generate internal name */
  internal_name = g_new0 (gchar, length+1);
  for (length = 0, p = name; *p != '\0'; ++p)
    if (g_ascii_isalnum (*p))
      internal_name[length++] = *p;
  internal_name[length] = '\0';

  /* Remember name for use by xfce4-mixer */
  g_object_set_data_full (G_OBJECT (mixer), "xfce-mixer-internal-name", internal_name, (GDestroyNotify) g_free);
  /* Keep the mixer (we want all devices to be visible) */
  return TRUE;
}

static const gchar *xfce_mixer_get_card_id (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);
  return g_object_get_data (G_OBJECT (card), "xfce-mixer-id");
}

static guint xfce_mixer_is_default_card (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);

  char cid[16];
  asound_get_default_card (cid);
  if (!strcmp (cid, xfce_mixer_get_card_id (card))) return 1;
  return 0;
}

static const gchar *xfce_mixer_get_card_display_name (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), NULL);
  return g_object_get_data (G_OBJECT (card), "xfce-mixer-name");
}

static gboolean xfce_mixer_is_bcm_device (GstElement *card)
{
  g_return_val_if_fail (GST_IS_MIXER (card), FALSE);
  if (strncmp (xfce_mixer_get_card_display_name (card), "bcm2835", 7) == 0) return TRUE;
  return FALSE;
}

static gboolean xfce_mixer_get_bcm_device_id (gchar *id)
{
    gint counter = 0;
    GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (xfce_mixer_is_bcm_device (iter->data))
        {
            if (id) strcpy (id, xfce_mixer_get_card_id (iter->data));
            g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
            return TRUE;
        }
    }
    if (mixers) g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
    return FALSE;
}

static void volumealsa_popup_set_position(GtkWidget * menu, gint * px, gint * py, gboolean * push_in, gpointer data)
{
    VolumeALSAPlugin * vol= (VolumeALSAPlugin *) data;

    /* Determine the coordinates. */
    lxpanel_plugin_popup_set_position_helper(vol->panel, vol->plugin, menu, px, py);
    *push_in = TRUE;
}

static void send_message (void)
{
  // to message the xfce mixer dialog, a Dbus connection is dropped and then reacquired
  // doing this the other (more logical) way around doesn't work, as Dbus doesn't pass the message fast enough
  static guint id = 0;
  if (id) g_bus_unown_name (id);
  id = g_bus_own_name (G_BUS_TYPE_SESSION, "org.lxde.volumealsa", 0, NULL, NULL, NULL, NULL, NULL);
}

static void set_default_card_event (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    stop_pulseaudio (vol);
    asound_set_default_card (widget->name);
    asound_restart (vol);
    volumealsa_update_display (vol);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static void set_bcm_output (GtkWidget * widget, VolumeALSAPlugin *vol)
{
    char cmdbuf[64], bcmdev[64];

    /* check that the BCM device is default... */
    stop_pulseaudio (vol);
    asound_get_default_card (cmdbuf);
    if (cmdbuf && xfce_mixer_get_bcm_device_id (bcmdev) && strcmp (cmdbuf, bcmdev))
    {
        /* ... and set it to default if not */
        asound_set_default_card (bcmdev);
        asound_restart (vol);

        /* set the output channel on the BCM device */
        sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
        system (cmdbuf);
    }
    else
    {
        /* set the output channel on the BCM device */
        sprintf (cmdbuf, "amixer cset numid=3 %s", widget->name);
        system (cmdbuf);
    }
    volumealsa_update_display (vol);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static gboolean validate_devices (VolumeALSAPlugin *vol)
{
    char device[32];
    snd_hctl_t *hctl;
    gint counter = 0;
    GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);

    /* Get the current setting - find cards, and which one is default */
    gboolean def_good = FALSE;
    for (iter = mixers; iter != NULL; iter = g_list_next (iter))
    {
        if (xfce_mixer_is_default_card (iter->data))
        {
            /* Check if we are still connected to the device */
            g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
            asound_get_default_card (device);
            if (snd_mixer_get_hctl (vol->mixer, device, &hctl))
            {
                asound_restart (vol);
                return FALSE;
            }
            else return TRUE;
        }
    }
    asound_find_valid_device ();
    asound_restart (vol);
    if (mixers) g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
    return FALSE;
}

static void open_config_dialog (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    volumealsa_configure (vol->panel, vol->plugin);
    gtk_menu_popdown (GTK_MENU(vol->menu_popup));
}

/* Handler for "focus-out" signal on popup window. */
static gboolean volumealsa_mouse_out (GtkWidget * widget, GdkEventButton * event, VolumeALSAPlugin * vol)
{
    /* Hide the widget. */
    gtk_widget_hide(vol->popup_window);
    vol->show_popup = FALSE;
    gdk_pointer_ungrab (GDK_CURRENT_TIME);
    return FALSE;
}

/* Handler for "button-press-event" signal on main widget. */

static gboolean volumealsa_button_press_event(GtkWidget * widget, GdkEventButton * event, LXPanel * panel)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(widget);

    if (vol->stopped) return TRUE;

    if (!validate_devices (vol)) volumealsa_update_display (vol);

    /* Left-click.  Show or hide the popup window. */
    if (event->button == 1)
    {
        if (vol->show_popup)
        {
            gtk_widget_hide(vol->popup_window);
            vol->show_popup = FALSE;
        }
        else
        {
            volumealsa_build_popup_window (vol->plugin);
            volumealsa_update_display (vol);

            gint x, y;
            gtk_window_set_position (GTK_WINDOW (vol->popup_window), GTK_WIN_POS_MOUSE);
            // need to draw the window in order to allow the plugin position helper to get its size
            gtk_widget_show_all (vol->popup_window);
            gtk_widget_hide (vol->popup_window);
            lxpanel_plugin_popup_set_position_helper (panel, widget, vol->popup_window, &x, &y);
            gdk_window_move (gtk_widget_get_window (vol->popup_window), x, y);
            gtk_window_present (GTK_WINDOW (vol->popup_window));
            gdk_pointer_grab (gtk_widget_get_window (vol->popup_window), TRUE, GDK_BUTTON_PRESS_MASK, NULL, NULL, GDK_CURRENT_TIME);
            g_signal_connect (G_OBJECT(vol->popup_window), "button-press-event", G_CALLBACK (volumealsa_mouse_out), vol);
            vol->show_popup = TRUE;
        }
    }

    /* Middle-click.  Toggle the mute status. */
    else if (event->button == 2)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(vol->mute_check), ! gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(vol->mute_check)));
    }
    else if (event->button == 3)
    {
        gint counter, devices;
        GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);
        GtkWidget *image, *mi;
        gboolean ext_dev = FALSE, bt_dev = TRUE;

        image = gtk_image_new ();
        set_icon (vol->panel, image, "dialog-ok-apply", 16);

        vol->menu_popup = gtk_menu_new ();

        if (xfce_mixer_get_bcm_device_id (NULL))
        {
            /* if the onboard card is default, find currently-set output */
            counter = -1;
            for (iter = mixers; iter != NULL; iter = g_list_next (iter))
            {
                if (xfce_mixer_is_default_card (iter->data) && xfce_mixer_is_bcm_device (iter->data))
                {
                    counter = asound_get_bcm_output ();
                    break;
                }
            }

            mi = gtk_image_menu_item_new_with_label (_("Analog"));
            gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);
            if (counter == 1)
            {
                gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
            }
            gtk_widget_set_name (mi, "1");
            g_signal_connect (mi, "activate", G_CALLBACK (set_bcm_output), (gpointer) vol);
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

            mi = gtk_image_menu_item_new_with_label (_("HDMI"));
            gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);
            if (counter == 2)
            {
                gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
            }
            gtk_widget_set_name (mi, "2");
            g_signal_connect (mi, "activate", G_CALLBACK (set_bcm_output), (gpointer) vol);
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

            bt_dev = FALSE;
            devices = 2;
        }
        else devices = 0;

        // add Bluetooth devices if PulseAudio is running...
        if (vol->objmanager)
        {
            char *btdevice = NULL;
            if (vol->con) get_bz_name_of_pa_sink (vol, &btdevice);

            // iterate all the objects the manager knows about
            GList *objects = g_dbus_object_manager_get_objects (vol->objmanager);
            while (objects != NULL)
            {
                GDBusObject *object = (GDBusObject *) objects->data;
                GList *interfaces = g_dbus_object_get_interfaces (object);
                while (interfaces != NULL)
                {
                    // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
                    GDBusInterface *interface = G_DBUS_INTERFACE (interfaces->data);
                    if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
                    {
                        GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                        GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                        GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                        GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                        DEVICE_TYPE dev = check_uuids (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface)));
                        if (name && icon && paired && trusted && dev == DEV_AUDIO_SINK && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                        {
                            if (!bt_dev)
                            {
                                mi = gtk_separator_menu_item_new ();
                                gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                                bt_dev = TRUE;
                            }
                            mi = gtk_image_menu_item_new_with_label (g_variant_get_string (name, NULL));
                            gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);

                            const char *devname = g_dbus_object_get_object_path (object);
                            if (btdevice)
                            {
                                if (!strncmp (btdevice + (strlen (btdevice) - 17), devname + (strlen (devname) - 17), 17))
                                {
                                    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
                                }
                            }
                            gtk_widget_set_name (mi, devname);  // use the widget name to store the card id
                            g_signal_connect (mi, "activate", G_CALLBACK (set_bt_card_event), (gpointer) vol);
                            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                            devices++;
                        }
                        g_variant_unref (name);
                        g_variant_unref (paired);
                        g_variant_unref (trusted);
                        break;
                    }
                    interfaces = interfaces->next;
                }
                objects = objects->next;
            }
        }

        // add external devices...
        for (iter = mixers; iter != NULL; iter = g_list_next (iter))
        {
            if (!xfce_mixer_is_bcm_device (iter->data))
            {
                if (!ext_dev && devices)
                {
                    mi = gtk_separator_menu_item_new ();
                    gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                }

                char namebuf[128];
                strcpy (namebuf, xfce_mixer_get_card_display_name (iter->data));
                namebuf[strlen(namebuf) - 13] = 0;
                mi = gtk_image_menu_item_new_with_label (namebuf);
                gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);

                if (vol->sink == -1 && xfce_mixer_is_default_card (iter->data))
                {
                    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
                }
                gtk_widget_set_name (mi, xfce_mixer_get_card_id (iter->data));  // use the widget name to store the card id
                g_signal_connect (mi, "activate", G_CALLBACK (set_default_card_event), (gpointer) vol);
                gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
                ext_dev = TRUE;
                devices++;
            }
        }

        if (ext_dev)
        {
            mi = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);

            mi = gtk_image_menu_item_new_with_label (_("USB Device Settings..."));
            g_signal_connect (mi, "activate", G_CALLBACK (open_config_dialog), (gpointer) vol);
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
        }

        if (!devices)
        {
            mi = gtk_image_menu_item_new_with_label (_("No audio devices found"));
            gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
            gtk_menu_shell_append (GTK_MENU_SHELL(vol->menu_popup), mi);
        }

        // lock menu if a dialog is open
        if (vol->conn_dialog)
        {
            GList *items = gtk_container_get_children (GTK_CONTAINER (vol->menu_popup));
            while (items)
            {
                gtk_widget_set_sensitive (GTK_WIDGET (items->data), FALSE);
                items = items->next;
            }
            g_list_free (items);
        }
    
        gtk_widget_show_all (vol->menu_popup);
        gtk_menu_popup (GTK_MENU(vol->menu_popup), NULL, NULL, (GtkMenuPositionFunc) volumealsa_popup_set_position, (gpointer) vol,
            event->button, event->time);
        if (mixers) g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
    }
    return TRUE;
}

static void volumealsa_theme_change(GtkWidget * widget, VolumeALSAPlugin * vol)
{
    set_icon (vol->panel, vol->tray_icon, vol->icon, 0);
}

/* Handler for "value_changed" signal on popup window vertical scale. */
static void volumealsa_popup_scale_changed(GtkRange * range, VolumeALSAPlugin * vol)
{
    /* Reflect the value of the control to the sound system. */
    if (vol->sink == -1)
    {
        if (!asound_is_muted (vol))
            asound_set_volume(vol, gtk_range_get_value(range));
    }
    else
    {
        if (!get_bt_mute (vol))
            set_bt_vol (vol, gtk_range_get_value (range) * 656);
    }

    /* Redraw the controls. */
    volumealsa_update_display(vol);
}

/* Handler for "scroll-event" signal on popup window vertical scale. */
static void volumealsa_popup_scale_scrolled(GtkScale * scale, GdkEventScroll * evt, VolumeALSAPlugin * vol)
{
    /* Get the state of the vertical scale. */
    gdouble val = gtk_range_get_value(GTK_RANGE(vol->volume_scale));

    /* Dispatch on scroll direction to update the value. */
    if ((evt->direction == GDK_SCROLL_UP) || (evt->direction == GDK_SCROLL_LEFT))
        val += 2;
    else
        val -= 2;

    /* Reset the state of the vertical scale.  This provokes a "value_changed" event. */
    gtk_range_set_value(GTK_RANGE(vol->volume_scale), CLAMP((int)val, 0, 100));
}

/* Handler for "toggled" signal on popup window mute checkbox. */
static void volumealsa_popup_mute_toggled(GtkWidget * widget, VolumeALSAPlugin * vol)
{
    /* Get the state of the mute toggle. */
    gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

    if (vol->sink == -1)
    {
        /* Reflect the mute toggle to the sound system. */
        if (vol->master_element != NULL)
        {
            int chn;
            for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++)
                snd_mixer_selem_set_playback_switch(vol->master_element, chn, ((active) ? 0 : 1));
        }
    }
    else
    {
        set_bt_mute (vol, active);
    }

    /* Redraw the controls. */
    volumealsa_update_display(vol);
}

/* Build the window that appears when the top level widget is clicked. */
static void volumealsa_build_popup_window(GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);

    if (vol->popup_window)
    {
        gtk_widget_destroy (vol->popup_window);
        vol->popup_window = NULL;
    }

    /* Create a new window. */
    vol->popup_window = gtk_window_new (GTK_WINDOW_POPUP);
    gtk_widget_set_name (vol->popup_window, "volals");
    gtk_window_set_decorated(GTK_WINDOW(vol->popup_window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(vol->popup_window), 5);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(vol->popup_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(vol->popup_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(vol->popup_window), GDK_WINDOW_TYPE_HINT_UTILITY);

    /* Create a scrolled window as the child of the top level window. */
    GtkWidget * scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name (scrolledwindow, "whitewd");
    gtk_container_set_border_width (GTK_CONTAINER(scrolledwindow), 0);
    gtk_widget_show(scrolledwindow);
    gtk_container_add(GTK_CONTAINER(vol->popup_window), scrolledwindow);
    gtk_widget_set_can_focus(scrolledwindow, FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolledwindow), GTK_SHADOW_NONE);

    /* Create a viewport as the child of the scrolled window. */
    GtkWidget * viewport = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolledwindow), viewport);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    gtk_widget_show(viewport);

    gtk_container_set_border_width(GTK_CONTAINER(vol->popup_window), 0);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolledwindow), GTK_SHADOW_IN);
    /* Create a vertical box as the child of the viewport. */
    GtkWidget * box = gtk_vbox_new (FALSE, 0);
    gtk_container_add(GTK_CONTAINER(viewport), box);

    /* Create a vertical scale as the child of the vertical box. */
    vol->volume_scale = gtk_vscale_new(GTK_ADJUSTMENT(gtk_adjustment_new(100, 0, 100, 0, 0, 0)));
    gtk_widget_set_name (vol->volume_scale, "volscale");
    g_object_set (vol->volume_scale, "height-request", 120, NULL);
    gtk_scale_set_draw_value(GTK_SCALE(vol->volume_scale), FALSE);
    gtk_range_set_inverted(GTK_RANGE(vol->volume_scale), TRUE);
    gtk_box_pack_start(GTK_BOX(box), vol->volume_scale, TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->volume_scale, FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler = g_signal_connect(vol->volume_scale, "value-changed", G_CALLBACK(volumealsa_popup_scale_changed), vol);
    g_signal_connect(vol->volume_scale, "scroll-event", G_CALLBACK(volumealsa_popup_scale_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->mute_check = gtk_check_button_new_with_label(_("Mute"));
    gtk_box_pack_end(GTK_BOX(box), vol->mute_check, FALSE, FALSE, 0);
    vol->mute_check_handler = g_signal_connect(vol->mute_check, "toggled", G_CALLBACK(volumealsa_popup_mute_toggled), vol);
    gtk_widget_set_can_focus (vol->mute_check, FALSE);

    /* Lock the controls if there is nothing to control... */
    if (vol->sink == -1)
    {
        gint counter = 0;
        GList *iter, *mixers = gst_audio_default_registry_mixer_filter (_xfce_mixer_filter_mixer, FALSE, &counter);

        gboolean def_good = FALSE;
        for (iter = mixers; iter != NULL; iter = g_list_next (iter))
        {
            if (xfce_mixer_is_default_card (iter->data))
            {
                def_good = TRUE;
                break;
            }
        }
        if (!def_good)
        {
            gtk_widget_set_sensitive (vol->volume_scale, FALSE);
            gtk_widget_set_sensitive (vol->mute_check, FALSE);
        }
        if (mixers) g_list_free_full (mixers, (GDestroyNotify) _xfce_mixer_destroy_mixer);
    }

    /* Set background to default. */
    //gtk_widget_set_style(viewport, panel_get_defstyle(vol->panel));
}

/* Plugin constructor. */
static GtkWidget *volumealsa_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context and set into Plugin private data pointer. */
    VolumeALSAPlugin * vol = g_new0(VolumeALSAPlugin, 1);
    GtkWidget *p;
    char buffer[128];
    FILE *fp;

    /* Initialise Gstreamer */
    gst_init (NULL, NULL);

    /* Initialize ALSA */
    asound_initialize (vol);

    /* Check the default device is valid and reset if not */
    validate_devices (vol);

    /* Allocate top level widget and set into Plugin widget pointer. */
    vol->panel = panel;
    vol->plugin = p = gtk_button_new();
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    //vol->plugin = p = gtk_event_box_new();
    g_signal_connect(vol->plugin, "button-press-event", G_CALLBACK(volumealsa_button_press_event), vol->panel);
    vol->settings = settings;
    lxpanel_plugin_set_data(p, vol, volumealsa_destructor);
    gtk_widget_add_events(p, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_tooltip_text(p, _("Volume control"));

    /* Allocate icon as a child of top level. */
    vol->tray_icon = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(p), vol->tray_icon);

    // fall back to ALSA until we can establish if Bluetooth is available
    vol->sink = -1;
    system ("pulseaudio --kill");
    configure_pa ();

    // set up callbacks to see if BlueZ is on DBus
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, cb_name_owned, cb_name_unowned, vol, NULL);

    /* Initialize window to appear when icon clicked. */
    volumealsa_build_popup_window(p);

    /* Connect signals. */
    g_signal_connect(G_OBJECT(p), "scroll-event", G_CALLBACK(volumealsa_popup_scale_scrolled), vol );
    g_signal_connect(panel_get_icon_theme(panel), "changed", G_CALLBACK(volumealsa_theme_change), vol );

    /* Update the display, show the widget, and return. */
    volumealsa_update_display(vol);
    gtk_widget_show_all(p);

    vol->stopped = FALSE;
    return p;
}

/* Plugin destructor. */
static void volumealsa_destructor(gpointer user_data)
{
    VolumeALSAPlugin * vol = (VolumeALSAPlugin *) user_data;

    asound_deinitialize(vol);

    /* If the dialog box is open, dismiss it. */
    if (vol->popup_window != NULL)
        gtk_widget_destroy(vol->popup_window);

    if (vol->restart_idle)
        g_source_remove(vol->restart_idle);

   if (vol->panel) /* SF bug #683: crash if constructor failed */
    g_signal_handlers_disconnect_by_func(panel_get_icon_theme(vol->panel),
                                         volumealsa_theme_change, vol);

    /* Deallocate all memory. */
    g_free(vol);
}

/* Callback when the configuration dialog is to be shown. */

static GtkWidget *volumealsa_configure(LXPanel *panel, GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);
    char *path = NULL;
    const gchar *command_line = NULL;
    GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;

    /* FIXME: configure settings! */
    /* check if command line was configured */
    config_setting_lookup_string(vol->settings, "MixerCommand", &command_line);
    /* FIXME: support "needs terminal" for MixerCommand */
    /* FIXME: selection for master channel! */
    /* FIXME: configure buttons for each action (toggle volume/mixer/mute)! */

    /* if command isn't set in settings then let guess it */
#if 0    
    if (command_line == NULL && (path = g_find_program_in_path("pulseaudio")))
    {
        g_free(path);
     /* Assume that when pulseaudio is installed, it's launching every time */
        if ((path = g_find_program_in_path("gnome-sound-applet")))
        {
            command_line = "gnome-sound-applet";
        }
        else if ((path = g_find_program_in_path("pavucontrol")))
        {
            command_line = "pavucontrol";
        }
    }
#endif
    /* Fallback to alsamixer when PA is not running, or when no PA utility is find */
    if (command_line == NULL)
    {
        if ((path = g_find_program_in_path("gnome-alsamixer")))
        {
            command_line = "gnome-alsamixer";
        }
        else if ((path = g_find_program_in_path("alsamixergui")))
        {
            command_line = "alsamixergui";
        }
        else if ((path = g_find_program_in_path("pimixer")))
        {
            command_line = "pimixer";
        }
        else if ((path = g_find_program_in_path("xfce4-mixer")))
        {
            command_line = "xfce4-mixer";
        }
       else if ((path = g_find_program_in_path("alsamixer")))
        {
            command_line = "alsamixer";
            flags = G_APP_INFO_CREATE_NEEDS_TERMINAL;
        }
    }
    g_free(path);

    if (command_line)
    {
        fm_launch_command_simple(NULL, NULL, flags, command_line, NULL);
    }
    else
    {
        fm_show_error(NULL, NULL,
                      _("Error, you need to install an application to configure"
                        " the sound (pavucontrol, alsamixer ...)"));
    }

    return NULL;
}

/* Callback when panel configuration changes. */
static void volumealsa_panel_configuration_changed(LXPanel *panel, GtkWidget *p)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(p);

    // the pulseaudio server might have been killed under us...
    if (!check_pulseaudio () && vol->sink != -1)
    {
        if (vol->sub_id) g_dbus_connection_signal_unsubscribe (vol->con, vol->sub_id);
        vol->sub_id = 0;
        system ("rm ~/.config/bt");
        g_object_unref (vol->con);
        vol->con = NULL;
        vol->sink = -1;
    }

    asound_restart(vol);
    volumealsa_build_popup_window (vol->plugin);
    /* Do a full redraw. */
    volumealsa_update_display(vol);
    if (vol->show_popup) gtk_widget_show_all (vol->popup_window);

}

static gboolean volumealsa_control_msg (GtkWidget *plugin, const char *cmd)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (plugin);

    if (!strcmp (cmd, "Start"))
    {
        asound_initialize (vol);
        g_warning ("volumealsa: Restarted ALSA interface...");
        volumealsa_update_display (vol);
        gtk_menu_popdown (GTK_MENU (vol->menu_popup));
        vol->stopped = FALSE;
        return TRUE;
    }

    if (!strcmp (cmd, "Stop"))
    {
        asound_deinitialize (vol);
        g_warning ("volumealsa: Stopped ALSA interface...");
        volumealsa_update_display (vol);
        gtk_menu_popdown (GTK_MENU (vol->menu_popup));
        vol->stopped = TRUE;
        return TRUE;
   }
   return FALSE;
}

FM_DEFINE_MODULE(lxpanel_gtk, volumealsa)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Volume Control (ALSA)"),
    .description = N_("Display and control volume for ALSA"),

    .new_instance = volumealsa_constructor,
    .config = volumealsa_configure,
    .reconfigure = volumealsa_panel_configuration_changed,
    .control = volumealsa_control_msg
};

/* vim: set sw=4 et sts=4 : */
