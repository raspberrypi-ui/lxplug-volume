/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
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

#define _ISOC99_SOURCE /* lrint() */
#define _GNU_SOURCE /* exp10() */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <alsa/asoundlib.h>
#include <poll.h>
#include <libfm/fm-gtk.h>

#include "plugin.h"

#define ICONS_TICK          PACKAGE_DATA_DIR "/images/dialog-ok-apply.png"

#define ICON_BUTTON_TRIM 4

#define DEBUG_ON
#ifdef DEBUG_ON
//#define DEBUG(fmt,args...) {FILE *flp = fopen ("/home/pi/vlog.txt", "ab+"); fprintf (flp, fmt,##args); fprintf (flp, "\n"); fclose (flp);}
#define DEBUG(fmt,args...) g_message("va: " fmt,##args)
#else
#define DEBUG
#endif

#ifdef __UCLIBC__
/* 10^x = 10^(log e^x) = (e^x)^log10 = e^(x * log 10) */
#define exp10(x) (exp((x) * log(10)))
#endif /* __UCLIBC__ */

#define MAX_LINEAR_DB_SCALE 24

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

    /* HDMI device names */
    guint hdmis;
    char *mon_names[2];

    GDBusObjectManager *objmanager;         /* BlueZ object manager */
    char *bt_conname;                       /* BlueZ name of device - just used during connection */
    GtkWidget *conn_dialog, *conn_label, *conn_ok;
} VolumeALSAPlugin;

static void send_message (void);
static gboolean asound_restart (gpointer vol_gpointer);
static gboolean asound_initialize (VolumeALSAPlugin *vol);
static void asound_deinitialize (VolumeALSAPlugin *vol);
static void volumealsa_update_display (VolumeALSAPlugin *vol);
static void volumealsa_destructor (gpointer user_data);
static void volumealsa_build_popup_window (GtkWidget *p);
static gboolean asound_is_default_card (int num);
static gboolean asound_is_bcm_device (int num);
static gboolean asound_get_bcm_device_id (gchar *id);
static gboolean asound_set_bcm_card (void);
static GtkWidget *volumealsa_configure (LXPanel *panel, GtkWidget *p);

static void asound_set_bt_device (char *devname);
static int asound_get_bt_device (char *id);
static gboolean asound_bt_is_default (void);
static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data);
static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data);
static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void bt_connect_device (VolumeALSAPlugin *vol);
static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data);
static void bt_disconnect_device (VolumeALSAPlugin *vol);
static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data);
static void set_bluetooth_output_device (GtkWidget *widget, VolumeALSAPlugin *vol);
static void show_connect_dialog (VolumeALSAPlugin *vol, gboolean failed, const gchar *param);
static void handle_close_connect_dialog (GtkButton *button, gpointer user_data);
static gint handle_delete_connect_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static gboolean bt_is_audio_sink (VolumeALSAPlugin *vol, const gchar *path);
static gboolean asound_is_current_bt_dev (const char *obj);

static long lrint_dir(double x, int dir);
static inline gboolean use_linear_dB_scale(long dBmin, long dBmax);
static double get_normalized_volume(snd_mixer_elem_t *elem, snd_mixer_selem_channel_id_t channel);
static int set_normalized_volume(snd_mixer_elem_t *elem, snd_mixer_selem_channel_id_t channel, double volume, int dir);

static void asound_get_default_card (char *id);
static void asound_set_default_card (const char *id);
static void asound_find_valid_device (void);
static int asound_get_bcm_output (void);
static int asound_get_simple_ctrls (int dev);

static char *get_string (char *cmd);
static int n_desktops (VolumeALSAPlugin *vol);

static void set_external_output_device (GtkWidget * widget, VolumeALSAPlugin * vol);
static void set_internal_output_device (GtkWidget * widget, VolumeALSAPlugin * vol);

/* General file parsing utils */

static char *get_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    int len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return g_strdup ("");
    if (getline (&line, &len, fp) > 0)
    {
        res = line;
        while (*res++) if (g_ascii_isspace (*res)) *res = 0;
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res ? res : g_strdup ("");
}

static int vsystem (const char *fmt, ...)
{
    char *cmdline;
    int res;

    va_list arg;
    va_start (arg, fmt);
    g_vasprintf (&cmdline, fmt, arg);
    va_end (arg);
    res = system (cmdline);
    g_free (cmdline);
    return res;
}

static gboolean find_in_section (char *file, char *sec, char *seek)
{
    char *cmd = g_strdup_printf ("sed -n '/%s/,/}/p' %s 2>/dev/null | grep -q %s", sec, file, seek);
    int res = system (cmd);
    g_free (cmd);
    if (res == 0) return TRUE;
    else return FALSE;
}


/* Bluetooth */

static void asound_set_bt_device (char *devname)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    int b1, b2, b3, b4, b5, b6;

    /* parse the device name to make sure it is valid */
    if (sscanf (devname, "/org/bluez/hci0/dev_%x_%x_%x_%x_%x_%x", &b1, &b2, &b3, &b4, &b5, &b6) != 6)
    {
        DEBUG ("Failed to set device - name %s invalid", devname);
        return;
    }

    /* check file exists - write default contents if not */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo 'pcm.!default {\n\ttype plug\n\tslave.pcm {\n\t\ttype bluealsa\n\t\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\n\t\tprofile \"a2dp\"\n\t}\n}\n\nctl.!default {\n\ttype bluealsa\n}\n' >> %s", b1, b2, b3, b4, b5, b6, user_config_file);
        g_free (user_config_file);
        return;
    }

    /* check for new pcm.default section */
    if (find_in_section (user_config_file, "pcm.!default", "'slave.pcm \".*\"'"))
    {
        /* replace slave.pcm "" section with bluealsa version */
        vsystem ("sed -i '/pcm.!default/,/}/ { s/slave.pcm .*/slave.pcm {\\n\\t\\ttype bluealsa\\n\\t\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\t\\tprofile \"a2dp\"\\n\\t}/ }' %s", b1, b2, b3, b4, b5, b6, user_config_file);
    }
    else if (find_in_section (user_config_file, "pcm.!default", "slave.pcm"))
    {
        /* replace slave.pcm {} section with bluealsa version */
        vsystem ("sed -i '/slave.pcm {/,/}/ { s/slave.pcm {/slave.pcm {\\n\\t\\ttype bluealsa\\n\\t\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\t\\tprofile \"a2dp\"\\n\\t}/; /slave.pcm/!d }' %s", b1, b2, b3, b4, b5, b6, user_config_file);
    }
    else
    {
        /* does the file contain an old format pcm.default section? */
        if (find_in_section (user_config_file, "pcm.!default", "type"))
        {
            /* overwrite entirety of pcm.default section with bluealsa version */
            vsystem ("sed -i '/pcm.!default/,/}/ { s/pcm.!default {/pcm.!default {\\n\\ttype bluealsa\\n\\tslave.pcm {\\n\\t\\ttype bluealsa\\n\\t\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\t\\tprofile \"a2dp\"\\n\\t}\\n}/; /pcm/!d }' %s", b1, b2, b3, b4, b5, b6, user_config_file);
        }
        else
        {
            /* append a pcm.default section */
            vsystem ("sed -i '$ a \\\n\\npcm.!default {\\n\\ttype plug\\n\\tslave.pcm {\\n\\t\\ttype bluealsa\\n\\t\\tdevice \"%02X:%02X:%02X:%02X:%02X:%02X\"\\n\\t\\tprofile\"a2dp\"\\n\\t}\\n}\\n %s", b1, b2, b3, b4, b5, b6, user_config_file);
        }
    }

    /* check for ctl.default section */
    if (find_in_section (user_config_file, "ctl.!default", "type"))
    {
        /* overwrite entirety of ctl.default section with bluealsa version */
        vsystem ("sed -i '/ctl.!default/,/}/ { s/ctl.!default {/ctl.!default {\\n\\ttype bluealsa\\n}/; /ctl/!d }' %s", user_config_file);
    }
    else
    {
        /* append a ctl.default section */
        vsystem ("sed -i '$ a \\\n\\nctl.!default {\\n\\ttype bluealsa\\n}\\n' %s", user_config_file);
    }

    g_free (user_config_file);
}

static int asound_get_bt_device (char *id)
{
    char *user_config_file, *cmd, *res;
    int ret = 0;

    user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

    cmd = g_strdup_printf ("sed -n '/pcm.!default/,/}/{/device/p}' %s 2>/dev/null | cut -d '\"' -f 2 | tr : _", user_config_file);
    res = get_string (cmd);
    g_free (cmd);

    if (res && res[0] && strlen (res) == 17)
    {
        strcpy (id, res);
        ret = 1;
    }

    g_free (res);
    return ret;
}

static gboolean asound_is_current_bt_dev (const char *obj)
{
    char device[20];
    if (asound_get_bt_device (device) && strstr (obj, device)) return TRUE;
    return FALSE;
}

static gboolean asound_bt_is_default (void)
{
    char buffer[32];
    asound_get_default_card (buffer);
    if (!strcmp (buffer, "bluealsa")) return TRUE;
    return FALSE;
}

static void bt_cb_object_added (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    if (asound_is_current_bt_dev (obj))
    {
        DEBUG ("Selected Bluetooth audio device has connected");
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }
}

static void bt_cb_object_removed (GDBusObjectManager *manager, GDBusObject *object, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    const char *obj = g_dbus_object_get_object_path (object);
    if (asound_is_current_bt_dev (obj))
    {
        DEBUG ("Selected Bluetooth audio device has disconnected");
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }
}

static void bt_cb_name_owned (GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    char device[20];
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
    else
    {
        /* register callbacks for devices being added or removed */
        g_signal_connect (vol->objmanager, "object-added", G_CALLBACK (bt_cb_object_added), vol);
        g_signal_connect (vol->objmanager, "object-removed", G_CALLBACK (bt_cb_object_removed), vol);
    }

    /* Check whether a Bluetooth audio device is the current default - connect to it if it is */
    if (asound_get_bt_device (device))
    {
        /* Reconnect the current Bluetooth audio device */
        if (vol->bt_conname) g_free (vol->bt_conname);
        vol->bt_conname = g_strdup_printf ("/org/bluez/hci0/dev_%s", device);

        DEBUG ("Connecting to %s...", vol->bt_conname);
        bt_connect_device (vol);
    }
}

static void bt_cb_name_unowned (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    DEBUG ("Name %s unowned on DBus", name);

    if (vol->objmanager) g_object_unref (vol->objmanager);
    vol->objmanager = NULL;
}

static void bt_connect_device (VolumeALSAPlugin *vol)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, vol->bt_conname, "org.bluez.Device1");
    if (interface)
    {
        // trust and connect
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "org.freedesktop.DBus.Properties.Set", g_variant_new ("(ssv)",
            g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "Trusted", g_variant_new_boolean (TRUE)), G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_trusted, vol);
        g_dbus_proxy_call (G_DBUS_PROXY (interface), "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_connected, vol);
        g_object_unref (interface);
    }
    else
    {
        DEBUG ("Couldn't get device interface from object manager");
        if (vol->conn_dialog) show_connect_dialog (vol, TRUE, _("Could not get BlueZ interface"));
    }
}

static void bt_cb_connected (GObject *source, GAsyncResult *res, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    GError *error = NULL;

    GVariant *var = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
    if (var) g_variant_unref (var);

    if (error)
    {
        DEBUG ("Connect error %s", error->message);

        // update dialog to show a warning
        if (vol->conn_dialog) show_connect_dialog (vol, TRUE, error->message);

        // call initialize to fall back to a non-BT device here if on initial startup
        if (asound_bt_is_default ()) asound_initialize (vol);
    }
    else
    {
        DEBUG ("Connected OK");

        // update asoundrc with connection details
        asound_set_bt_device (vol->bt_conname);

        // reinit alsa to configure mixer
        asound_initialize (vol);

        // close the connection dialog
        handle_close_connect_dialog (NULL, vol);
    }

    // delete the connection information
    g_free (vol->bt_conname);
    vol->bt_conname = NULL;

    // update the display whether we succeeded or not, as a failure will have caused a fallback to ALSA
    volumealsa_update_display (vol);
    if (vol->menu_popup) gtk_menu_popdown (GTK_MENU (vol->menu_popup));
    send_message ();
}

static void bt_cb_trusted (GObject *source, GAsyncResult *res, gpointer user_data)
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

static void bt_disconnect_device (VolumeALSAPlugin *vol)
{
    // get the name of the device with the current sink number
    GError *error = NULL;
    char buffer[64], device[20];

    if (asound_get_bt_device (device))
    {
        sprintf (buffer, "/org/bluez/hci0/dev_%s", device);
        DEBUG ("Device to disconnect = %s", buffer);

        // call the disconnect method on BlueZ
        if (vol->objmanager)
        {
            GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, buffer, "org.bluez.Device1");
            if (interface)
            {
                DEBUG ("Disconnecting...");
                g_dbus_proxy_call (G_DBUS_PROXY (interface), "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, bt_cb_disconnected, vol);
                g_object_unref (interface);
                return;
            }
        }
    }

    // if no connection found, just make the new connection
    if (vol->bt_conname)
    {
        DEBUG ("Nothing to disconnect - connecting to %s...", vol->bt_conname);
        bt_connect_device (vol);
    }
}

static void bt_cb_disconnected (GObject *source, GAsyncResult *res, gpointer user_data)
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
    if (vol->bt_conname)
    {
        // mixer and element handles will now be invalid
        vol->mixer = NULL;
        vol->master_element = NULL;

        DEBUG ("Connecting to %s...", vol->bt_conname);
        bt_connect_device (vol);
    }
}

static void set_bluetooth_output_device (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    // show the connection dialog
    show_connect_dialog (vol, FALSE, gtk_menu_item_get_label (GTK_MENU_ITEM (widget)));

    // store the name of the BlueZ device to connect to once the disconnect has happened
    if (vol->bt_conname) g_free (vol->bt_conname);
    vol->bt_conname = g_strndup (widget->name, 64);

    bt_disconnect_device (vol);
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
        g_signal_connect (GTK_OBJECT (vol->conn_dialog), "delete_event", G_CALLBACK (handle_delete_connect_dialog), vol);
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

static gint handle_delete_connect_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    VolumeALSAPlugin *vol = (VolumeALSAPlugin *) user_data;
    if (vol->conn_dialog)
    {
        gtk_widget_destroy (vol->conn_dialog);
        vol->conn_dialog = NULL;
    }
    return TRUE;
}

static gboolean bt_is_audio_sink (VolumeALSAPlugin *vol, const gchar *path)
{
    GDBusInterface *interface = g_dbus_object_manager_get_interface (vol->objmanager, path, "org.bluez.Device1");
    GVariant *elem, *var = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "UUIDs");
    GVariantIter iter;
    g_variant_iter_init (&iter, var);
    while (elem = g_variant_iter_next_value (&iter))
    {
        const char *uuid = g_variant_get_string (elem, NULL);
        if (!strncasecmp (uuid, "00001124", 8)) return FALSE;
        if (!strncasecmp (uuid, "0000110B", 8)) return TRUE;
        g_variant_unref (elem);
    }
    g_variant_unref (var);
    g_object_unref (interface);
    return FALSE;
}

/*** ALSA ***/

static long lrint_dir(double x, int dir)
{
    if (dir > 0)
        return lrint(ceil(x));
    else if (dir < 0)
        return lrint(floor(x));
    else
        return lrint(x);
}

static inline gboolean use_linear_dB_scale(long dBmin, long dBmax)
{
    return dBmax - dBmin <= MAX_LINEAR_DB_SCALE * 100;
}

static double get_normalized_volume(snd_mixer_elem_t *elem,
                    snd_mixer_selem_channel_id_t channel)
{
    long min, max, value;
    double normalized, min_norm;
    int err;

    err = snd_mixer_selem_get_playback_dB_range(elem, &min, &max);
    if (err < 0 || min >= max) {
        err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        if (err < 0 || min == max)
            return 0;

        err = snd_mixer_selem_get_playback_volume(elem, channel, &value);
        if (err < 0)
            return 0;

        return (value - min) / (double)(max - min);
    }

    err = snd_mixer_selem_get_playback_dB(elem, channel, &value);
    if (err < 0)
        return 0;

    if (use_linear_dB_scale(min, max))
        return (value - min) / (double)(max - min);

    normalized = exp10((value - max) / 6000.0);
    if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
        min_norm = exp10((min - max) / 6000.0);
        normalized = (normalized - min_norm) / (1 - min_norm);
    }

    return normalized;
}

static int set_normalized_volume(snd_mixer_elem_t *elem,
                 snd_mixer_selem_channel_id_t channel,
                 double volume,
                 int dir)
{
    long min, max, value;
    double min_norm;
    int err;

    err = snd_mixer_selem_get_playback_dB_range(elem, &min, &max);
    if (err < 0 || min >= max) {
        err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        if (err < 0)
            return err;

        value = lrint_dir(volume * (max - min), dir) + min;
        return snd_mixer_selem_set_playback_volume(elem, channel, value);
    }

    if (use_linear_dB_scale(min, max)) {
        value = lrint_dir(volume * (max - min), dir) + min;
        return snd_mixer_selem_set_playback_dB(elem, channel, value, dir);
    }

    if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
        min_norm = exp10((min - max) / 6000.0);
        volume = volume * (1 - min_norm) + min_norm;
    }
    value = lrint_dir(6000.0 * log10(volume), dir) + max;
    return snd_mixer_selem_set_playback_dB(elem, channel, value, dir);
}

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
            if (!strncasecmp (name, "Master", 6)) return TRUE;
            if (!strncasecmp (name, "Front", 5)) return TRUE;
            if (!strncasecmp (name, "PCM", 3)) return TRUE;
            if (!strncasecmp (name, "LineOut", 7)) return TRUE;
            if (!strncasecmp (name, "Digital", 7)) return TRUE;
            if (!strncasecmp (name, "Headphone", 9)) return TRUE;
            if (!strncasecmp (name, "Speaker", 7)) return TRUE;
            if (!strncasecmp (name + strlen(name) - 4, "a2dp", 4)) return TRUE;
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
static gboolean asound_mixer_event (GIOChannel *channel, GIOCondition cond, gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = (VolumeALSAPlugin *) vol_gpointer;
    int res = 0;

    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    if (vol->mixer_evt_idle == 0)
    {
        vol->mixer_evt_idle = g_idle_add_full(G_PRIORITY_DEFAULT, (GSourceFunc) asound_reset_mixer_evt_idle, vol, NULL);
        if (vol->mixer) res = snd_mixer_handle_events(vol->mixer);
    }

    if (cond & G_IO_IN)
    {
        /* the status of mixer is changed. update of display is needed. */
        /* don't do this if res > 1, as that seems to happen if a BT device has disconnected... */
        if (res < 2) volumealsa_update_display(vol);
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

static gboolean asound_restart (gpointer vol_gpointer)
{
    VolumeALSAPlugin * vol = vol_gpointer;

    if (!g_main_current_source ()) return TRUE;
    if (g_source_is_destroyed (g_main_current_source ()))
        return FALSE;

    asound_deinitialize (vol);

    if (!asound_initialize (vol))
    {
        g_warning ("volumealsa: Re-initialization failed.");
        return TRUE; // try again in a second
    }

    g_warning ("volumealsa: Restarted ALSA interface...");

    vol->restart_idle = 0;
    return FALSE;
}

static void asound_get_default_card (char *id)
{
    char *cmd, *res, *res2;
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);

    /* first check to see if Bluetooth is in use */
    cmd = g_strdup_printf ("sed -n '/pcm.!default/,/}/{/bluealsa/p}' %s 2>/dev/null", user_config_file);
    res = get_string (cmd);
    g_free (cmd);

    if (res[0]) sprintf (id, "bluealsa");
    else
    {
        /* if not, check for new format file */
        g_free (res);
        cmd = g_strdup_printf ("sed -n '/pcm.!default/,/}/{/slave.pcm/p}' %s 2>/dev/null | cut -d '\"' -f 2", user_config_file);
        res = get_string (cmd);
        g_free (cmd);

        if (res[0]) strcpy (id, res);
        else
        {
            /* if not, check for old format file */
            g_free (res);
            cmd = g_strdup_printf ("sed -n '/pcm.!default/,/}/{/type/p}' %s 2>/dev/null | cut -d ' ' -f 2", user_config_file);
            res = get_string (cmd);
            g_free (cmd);

            cmd = g_strdup_printf ("sed -n '/pcm.!default/,/}/{/card/p}' %s 2>/dev/null | cut -d ' ' -f 2", user_config_file);
            res2 = get_string (cmd);
            g_free (cmd);

            if (res[0] && res2[0]) sprintf (id, "%s:%s", res, res2);
            else sprintf (id, "hw:0");
            g_free (res);
            g_free (res2);
        }
    }
    g_free (user_config_file);
}

static void asound_set_default_card (const char *id)
{
    char *user_config_file = g_build_filename (g_get_home_dir (), "/.asoundrc", NULL);
    char idbuf[16], *card;

    /* break the id string into the type (before the colon) and the card number (after the colon) */
    strcpy (idbuf, id);
    card = strchr (idbuf, ':') + 1;
    *(strchr (idbuf, ':')) = 0;

    /* check file exists - write default contents if not */
    if (!g_file_test (user_config_file, G_FILE_TEST_IS_REGULAR))
    {
        vsystem ("echo 'pcm.!default {\n\ttype plug\n\tslave.pcm \"%s:%s\"\n}\n\nctl.!default {\n\ttype %s\n\tcard %s\n}\n' >> %s", idbuf, card, idbuf, card, user_config_file);
        g_free (user_config_file);
        return;
    }

    /* check for new pcm.default section */
    if (find_in_section (user_config_file, "pcm.!default", "'slave.pcm \".*\"'"))
    {
        /* file is in new format already, so update in place */
        vsystem ("sed -i '/pcm.!default/,/}/ { s/slave.pcm .*/slave.pcm \"%s:%s\"/ }' %s", idbuf, card, user_config_file);
    }
    else if (find_in_section (user_config_file, "pcm.!default", "slave.pcm"))
    {
        /* replace type in pcm section with type plug */
        vsystem ("sed -i '/pcm.!default/,/}/ s/type .*/type plug/' %s", user_config_file);

        /* replace slave.pcm {} section with slave.pcm "card ID" */
        vsystem ("sed -i '/slave.pcm {/,/}/ { s/slave.pcm {/slave.pcm \"%s:%s\"/; /slave.pcm/!d }' %s", idbuf, card, user_config_file);
    }
    else
    {
        /* does the file contain an old format pcm.default section? */
        if (find_in_section (user_config_file, "pcm.!default", "type") && find_in_section (user_config_file, "pcm.!default", "card"))
        {
            /* old format section found; update it to the new format */
            vsystem ("sed -i '/pcm.!default/,/}/ { s/type .*/type plug\\n\\tslave.pcm \"%s:%s\"/ }' %s", idbuf, card, user_config_file);
            vsystem ("sed -i '/pcm.!default/,/}/ { /card .*/d }' %s", user_config_file);
        }
        else
        {
            /* append a pcm.default section in the new format */
            vsystem ("sed -i '$ a \\\n\\npcm.!default {\\n\\ttype plug\\n\\tslave.pcm \"%s:%s\"\\n}\\n' %s", idbuf, card, user_config_file);
        }
    }

    /* check for ctl.default section */
    if (find_in_section (user_config_file, "ctl.!default", "type"))
    {
        if (find_in_section (user_config_file, "ctl.!default", "card"))
        {
            /* standard ctl.default section found; update both type and card */
            vsystem ("sed -i '/ctl.!default/,/}/ { s/type .*/type %s/g; s/card .*/card %s/g; }' %s", idbuf, card, user_config_file);
        }
        else
        {
            /* ctl has type but not card - probably bluetooth then, so replace type and add card */
            vsystem ("sed -i '/ctl.!default/,/}/ { s/type .*/type %s\\n\\tcard %s/g; }' %s", idbuf, card, user_config_file);
        }
    }
    else
    {
        /* append a ctl.default section */
        vsystem ("sed -i '$ a \\\n\\nctl.!default {\\n\\ttype %s\\n\\tcard %s\\n}\\n' %s", idbuf, card, user_config_file);
    }

    g_free (user_config_file);
}

static gboolean asound_set_bcm_card (void)
{
    char bcmdev[32];
    if (asound_get_bcm_device_id (bcmdev))
    {
        asound_set_default_card (bcmdev);
        return TRUE;
    }
    return FALSE;
}

static int asound_get_bcm_output (void)
{
    char *res;
    int n, val = -1;

    res = get_string ("amixer cget numid=3 2>/dev/null | grep : | cut -d = -f 2");
    if (sscanf (res, "%d", &n) == 1) val = n;
    g_free (res);

    if (val == 0)
    {
        /* set to analog if result is auto */
        system ("amixer -q cset numid=3 2");
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
        int num = -1;
        char buf[16];

        g_warning ("volumealsa: Internal device not available - looking for first valid ALSA device...");
        while (1)
        {
            if (snd_card_next (&num) < 0)
            {
                g_warning ("volumealsa: Cannot enumerate devices");
                break;
            }
            if (num == -1) break;

            sprintf (buf, "hw:%d", num);
            g_warning ("volumealsa: Valid ALSA device %s found", buf);
            asound_set_default_card (buf);
            return;
        }
        g_warning ("volumealsa: No ALSA devices found");
    }
}

/* Initialize the ALSA interface. */
static gboolean asound_initialize (VolumeALSAPlugin * vol)
{
    char device[32];

    // make sure existing watches are removed by calling deinit
    asound_deinitialize (vol);

    asound_get_default_card (device);

    /* Access the "default" device. */
    snd_mixer_open (&vol->mixer, 0);
    if (snd_mixer_attach (vol->mixer, device))
    {
        g_warning ("volumealsa: Couldn't attach mixer - looking for another valid device");
        asound_find_valid_device ();
        asound_get_default_card (device);
        snd_mixer_attach (vol->mixer, device);
    }
    snd_mixer_selem_register (vol->mixer, NULL, NULL);
    snd_mixer_load (vol->mixer);

    /* Find Master element, or Front element, or PCM element, or LineOut element.
     * If one of these succeeds, master_element is valid. */
    asound_find_elements (vol);

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

    if (asound_get_simple_ctrls (-1) == -1)
    {
        vol->mixer = NULL;
        vol->master_element = NULL;
        asound_set_default_card ("hw:-1");
    }

    return TRUE;
}

static void asound_deinitialize (VolumeALSAPlugin * vol)
{
    guint i;

    if (vol->mixer_evt_idle != 0)
    {
        g_source_remove (vol->mixer_evt_idle);
        vol->mixer_evt_idle = 0;
    }
    for (i = 0; i < vol->num_channels; i++)
    {
        g_source_remove (vol->watches[i]);
        g_io_channel_shutdown (vol->channels[i], FALSE, NULL);
        g_io_channel_unref (vol->channels[i]);
    }
    g_free (vol->channels);
    g_free (vol->watches);
    vol->channels = NULL;
    vol->watches = NULL;
    vol->num_channels = 0;

    if (vol->mixer)
    {
        char device[32];
        asound_get_default_card (device);
        if (*device) snd_mixer_detach (vol->mixer, device);
        snd_mixer_close (vol->mixer);
    }
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

static void asound_set_mute (VolumeALSAPlugin * vol, gboolean mute)
{
    if (vol->master_element != NULL)
    {
        int chn;
        for (chn = 0; chn <= SND_MIXER_SCHN_LAST; chn++)
            snd_mixer_selem_set_playback_switch(vol->master_element, chn, mute ? 0 : 1);
    }
}

/* Get the volume from the sound system.
 * This implementation returns the average of the Front Left and Front Right channels. */
static int asound_get_volume(VolumeALSAPlugin * vol)
{
    double aleft = 0;
    double aright = 0;
    if (vol->master_element != NULL)
    {
        aleft = get_normalized_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT);
        aright = get_normalized_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT);
    }
    return (int)round((aleft + aright) * 50);
}

/* Set the volume to the sound system.
 * This implementation sets the Front Left and Front Right channels to the specified value. */
static void asound_set_volume(VolumeALSAPlugin * vol, int volume)
{
    int dir = volume - asound_get_volume(vol);
    double vol_perc = (double)volume / 100;

    if (vol->master_element != NULL)
    {
        set_normalized_volume(vol->master_element, SND_MIXER_SCHN_FRONT_LEFT, vol_perc, dir);
        set_normalized_volume(vol->master_element, SND_MIXER_SCHN_FRONT_RIGHT, vol_perc, dir);
    }
}

/*** Graphics ***/

static void volumealsa_update_current_icon(VolumeALSAPlugin * vol)
{
    gboolean mute;
    int level;

    /* Mute status. */
    mute = asound_is_muted(vol);
    level = asound_get_volume(vol);

    /* Change icon according to mute / volume */
    const char* icon="audio-volume-muted";
    if (mute)
    {
         icon="audio-volume-muted";
    }
    else if (level >= 66)
    {
         icon="audio-volume-high";
    }
    else if (level >= 33)
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

#ifdef ENABLE_NLS
    // need to rebind here for tooltip update
    textdomain ( GETTEXT_PACKAGE );
#endif

    /* Mute status. */
    mute = asound_is_muted(vol);
    level = asound_get_volume(vol);
    if (mute) level = 0;

    volumealsa_update_current_icon(vol);

    /* Change icon, fallback to default icon if theme doesn't exist */
    set_icon (vol->panel, vol->tray_icon, vol->icon, 0);

    g_signal_handler_block(vol->mute_check, vol->mute_check_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(vol->mute_check), mute);
    gtk_widget_set_sensitive(vol->mute_check, asound_has_mute(vol));
    g_signal_handler_unblock(vol->mute_check, vol->mute_check_handler);

    /* Volume. */
    if (vol->volume_scale != NULL)
    {
        g_signal_handler_block(vol->volume_scale, vol->volume_scale_handler);
        gtk_range_set_value(GTK_RANGE(vol->volume_scale), level);
        g_signal_handler_unblock(vol->volume_scale, vol->volume_scale_handler);
    }

    /* Display current level in tooltip. */
    char *tooltip = g_strdup_printf("%s %d", _("Volume control"), level);
    gtk_widget_set_tooltip_text (vol->plugin, tooltip);
    g_free (tooltip);
}

/* ALSA device ID helper functions */

static gboolean asound_is_default_card (int num)
{
    char cid[32], buf[16];

    sprintf (buf, "hw:%d", num);
    asound_get_default_card (cid);
    if (!strcmp (cid, buf)) return TRUE;
    return FALSE;
}

static gboolean asound_is_bcm_device (int num)
{
    char *name;
    if (snd_card_get_name (num, &name)) return FALSE;
    int res = strncmp (name, "bcm2835", 7);
    g_free (name);
    if (res) return FALSE;
    return TRUE;
}

static gboolean asound_get_bcm_device_id (gchar *id)
{
    int num = -1;

    while (1)
    {
        if (snd_card_next (&num) < 0)
        {
            g_warning ("volumealsa: Cannot enumerate devices");
            break;
        }
        if (num == -1) break;

        if (asound_is_bcm_device (num))
        {
            if (id) sprintf (id, "hw:%d", num);
            return TRUE;
        }
    }
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

static void set_external_output_device (GtkWidget * widget, VolumeALSAPlugin * vol)
{
    /* if there is a Bluetooth device in use, disconnect it first */
    bt_disconnect_device (vol);

    asound_set_default_card (widget->name);
    asound_restart (vol);
    volumealsa_update_display (vol);
    if (vol->menu_popup) gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

static void set_internal_output_device (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    char defdev[32], bcmdev[32];

    /* if there is a Bluetooth device in use, disconnect it first */
    bt_disconnect_device (vol);

    /* check that the BCM device is default... */
    asound_get_default_card (defdev);
    if (asound_get_bcm_device_id (bcmdev) && strcmp (defdev, bcmdev))
    {
        /* ... and set it to default if not */
        asound_set_default_card (bcmdev);
        asound_restart (vol);
    }

    /* set the output channel on the BCM device */
    char *cmd = g_strdup_printf ("amixer -q cset numid=3 %s 2>/dev/null", widget->name);
    system (cmd);
    g_free (cmd);

    volumealsa_update_display (vol);
    if (vol->menu_popup) gtk_menu_popdown (GTK_MENU(vol->menu_popup));
    send_message ();
}

/* Multiple HDMI support */

static int n_desktops (VolumeALSAPlugin *vol)
{
    int i, n, m;
    char *res;

    /* check xrandr for connected monitors */
    res = get_string ("xrandr -q | grep -c connected");
    n = sscanf (res, "%d", &m);
    g_free (res);
    if (n != 1 || m <= 0) m = 1;
    if (m > 2) m = 2;

    /* get the names */
    if (m == 2)
    {
        for (i = 0; i < m; i++)
        {
            res = g_strdup_printf ("xrandr --listmonitors | grep %d: | cut -d ' ' -f 6", i);
            vol->mon_names[i] = get_string (res);
            g_free (res);
        }

        /* check both devices are HDMI */
        if ((vol->mon_names[0] && strncmp (vol->mon_names[0], "HDMI", 4) != 0)
            || (vol->mon_names[1] && strncmp (vol->mon_names[1], "HDMI", 4) != 0))
                m = 1;
    }

    return m;
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

/* This function is used for two things - for finding out how many controls are on the current device,
 * and for finding out if the current device is still valid (i.e. not disconnected). If it returns -1,
 * amixer info has returned no data, so the device is invalid. If it returns 0 or greater, the current
 * device is valid and has the returned number of controls. */

static int asound_get_simple_ctrls (int dev)
{
    char *cmd, *res;
    int n, m;

    if (dev == -1)
        cmd = g_strdup_printf ("amixer info 2>/dev/null | grep \"Simple ctrls\" | cut -d: -f2 | tr -d ' '");
    else
        cmd = g_strdup_printf ("amixer -c %d info 2>/dev/null | grep \"Simple ctrls\" | cut -d: -f2 | tr -d ' '", dev);

    res = get_string (cmd);
    g_free (cmd);

    n = sscanf (res, "%d", &m);
    g_free (res);

    if (n == 1) return m;
    return -1;
}

/* Handler for "button-press-event" signal on main widget. */

static GtkWidget *volumealsa_menu_item_add (VolumeALSAPlugin *vol, const char *label, const char *name, gboolean selected, GCallback cb)
{
    GtkWidget *mi = gtk_image_menu_item_new_with_label (label);
    gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (mi), TRUE);
    if (selected)
    {
        GtkWidget *image = gtk_image_new ();
        set_icon (vol->panel, image, "dialog-ok-apply", panel_get_icon_size (vol->panel) > 36 ? 24 : 16);
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM(mi), image);
    }
    gtk_widget_set_name (mi, name);
    g_signal_connect (mi, "activate", cb, (gpointer) vol);
    gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
    return mi;
}

static gboolean volumealsa_button_press_event(GtkWidget * widget, GdkEventButton * event, LXPanel * panel)
{
    VolumeALSAPlugin * vol = lxpanel_plugin_get_data(widget);

#ifdef ENABLE_NLS
    textdomain ( GETTEXT_PACKAGE );
#endif
    if (vol->stopped) return TRUE;

    int ctrls = asound_get_simple_ctrls (-1);
    if (ctrls < 0)
    {
        vol->mixer = NULL;
        vol->master_element = NULL;
        asound_set_default_card ("hw:-1");
        volumealsa_update_display (vol);
    }

    if (ctrls > 0 && vol->master_element == NULL)
    {
        // reconnect a BT device that has connected since startup...
        asound_initialize (vol);
        volumealsa_update_display (vol);
    }

    if (event->button == 1)
    {
        /* left-click - show or hide volume popup */
        if (asound_get_simple_ctrls (-1) < 1)
        {
            GtkWidget *mi;
            vol->menu_popup = gtk_menu_new ();
            mi = gtk_menu_item_new_with_label (_("No volume control on this device"));
            gtk_widget_set_sensitive (mi, FALSE);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
            gtk_widget_show_all (vol->menu_popup);
            gtk_menu_popup (GTK_MENU (vol->menu_popup), NULL, NULL, (GtkMenuPositionFunc) volumealsa_popup_set_position, (gpointer) vol,
                event->button, event->time);
            return TRUE;
        }

        if (vol->show_popup)
        {
            gtk_widget_hide (vol->popup_window);
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
            g_signal_connect (G_OBJECT (vol->popup_window), "button-press-event", G_CALLBACK (volumealsa_mouse_out), vol);
            vol->show_popup = TRUE;
        }
    }
    else if (event->button == 2)
    {
        /* middle-click - toggle mute */
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vol->mute_check), ! gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (vol->mute_check)));
    }
    else if (event->button == 3)
    {
        /* right-click - show device list */
        GtkWidget *mi;
        gint devices = 0, card_num;
        gboolean ext_dev = FALSE, bt_dev = FALSE;

        vol->menu_popup = gtk_menu_new ();

        /* add internal device... */
        card_num = -1;
        while (1)
        {
            if (snd_card_next (&card_num) < 0)
            {
                g_warning ("volumealsa: Cannot enumerate devices");
                break;
            }
            if (card_num == -1) break;

            if (asound_is_bcm_device (card_num))
            {
                /* if the onboard card is default, find currently-set output */
                gint bcm = 0;

                if (asound_is_default_card (card_num)) bcm = asound_get_bcm_output ();

                volumealsa_menu_item_add (vol, _("Analog"), "1", bcm == 1, G_CALLBACK (set_internal_output_device));
                if (vol->hdmis == 2)
                {
                    volumealsa_menu_item_add (vol, vol->mon_names[0], "2", bcm == 2, G_CALLBACK (set_internal_output_device));
                    volumealsa_menu_item_add (vol, vol->mon_names[1], "3", bcm == 3, G_CALLBACK (set_internal_output_device));
                    devices = 3;
                }
                else
                {
                    volumealsa_menu_item_add (vol, _("HDMI"), "2", bcm == 2, G_CALLBACK (set_internal_output_device));
                    devices = 2;
                }
                break;
            }
        }

        // add Bluetooth devices...
        if (vol->objmanager)
        {
            // iterate all the objects the manager knows about
            GList *objects = g_dbus_object_manager_get_objects (vol->objmanager);
            while (objects != NULL)
            {
                GDBusObject *object = (GDBusObject *) objects->data;
                const char *objpath = g_dbus_object_get_object_path (object);
                GList *interfaces = g_dbus_object_get_interfaces (object);
                while (interfaces != NULL)
                {
                    // if an object has a Device1 interface, it is a Bluetooth device - add it to the list
                    GDBusInterface *interface = G_DBUS_INTERFACE (interfaces->data);
                    if (g_strcmp0 (g_dbus_proxy_get_interface_name (G_DBUS_PROXY (interface)), "org.bluez.Device1") == 0)
                    {
                        if (bt_is_audio_sink (vol, g_dbus_proxy_get_object_path (G_DBUS_PROXY (interface))))
                        {
                            GVariant *name = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Alias");
                            GVariant *icon = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Icon");
                            GVariant *paired = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Paired");
                            GVariant *trusted = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (interface), "Trusted");
                            if (name && icon && paired && trusted && g_variant_get_boolean (paired) && g_variant_get_boolean (trusted))
                            {
                                if (!bt_dev && devices)
                                {
                                    mi = gtk_separator_menu_item_new ();
                                    gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
                                }

                                volumealsa_menu_item_add (vol, g_variant_get_string (name, NULL), objpath, asound_is_current_bt_dev (objpath), G_CALLBACK (set_bluetooth_output_device));
                                bt_dev = TRUE;
                                devices++;
                            }
                            g_variant_unref (name);
                            g_variant_unref (icon);
                            g_variant_unref (paired);
                            g_variant_unref (trusted);
                        }
                        break;
                    }
                    interfaces = interfaces->next;
                }
                objects = objects->next;
            }
        }

        // add external devices...
        card_num = -1;
        while (1)
        {
            if (snd_card_next (&card_num) < 0)
            {
                g_warning ("volumealsa: Cannot enumerate devices");
                break;
            }
            if (card_num == -1) break;

            if (!asound_is_bcm_device (card_num))
            {
                char *nam, *dev;
                snd_card_get_name (card_num, &nam);
                dev = g_strdup_printf ("hw:%d", card_num);

                if (!ext_dev && devices)
                {
                    mi = gtk_separator_menu_item_new ();
                    gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
                }

                mi = volumealsa_menu_item_add (vol, nam, dev, asound_is_default_card (card_num), G_CALLBACK (set_external_output_device));
                if (asound_get_simple_ctrls (card_num) < 1)
                {
                    char *lab = g_strdup_printf ("<i>%s</i>", nam);
                    gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child (GTK_BIN (mi))), lab);
                    gtk_widget_set_tooltip_text (mi, _("No volume control on this device"));
                    g_free (lab);
                }

                g_free (nam);
                g_free (dev);
                ext_dev = TRUE;
                devices++;
            }
        }

        if (ext_dev)
        {
            mi = gtk_separator_menu_item_new ();
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);

            mi = gtk_image_menu_item_new_with_label (_("USB Device Settings..."));
            g_signal_connect (mi, "activate", G_CALLBACK (open_config_dialog), (gpointer) vol);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
        }

        if (!devices)
        {
            mi = gtk_image_menu_item_new_with_label (_("No audio devices found"));
            gtk_widget_set_sensitive (GTK_WIDGET (mi), FALSE);
            gtk_menu_shell_append (GTK_MENU_SHELL (vol->menu_popup), mi);
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
    }
    return TRUE;
}

static void volumealsa_theme_change (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    set_icon (vol->panel, vol->tray_icon, vol->icon, 0);
}

/* Handler for "value_changed" signal on popup window vertical scale. */
static void volumealsa_popup_scale_changed (GtkRange *range, VolumeALSAPlugin *vol)
{
    /* Reflect the value of the control to the sound system. */
    if (!asound_is_muted (vol))
        asound_set_volume (vol, gtk_range_get_value (range));

    /* Redraw the controls. */
    volumealsa_update_display (vol);
}

/* Handler for "scroll-event" signal on popup window vertical scale. */
static void volumealsa_popup_scale_scrolled (GtkScale *scale, GdkEventScroll *evt, VolumeALSAPlugin *vol)
{
    /* Get the state of the vertical scale. */
    gdouble val = gtk_range_get_value (GTK_RANGE (vol->volume_scale));

    /* Dispatch on scroll direction to update the value. */
    if ((evt->direction == GDK_SCROLL_UP) || (evt->direction == GDK_SCROLL_LEFT))
        val += 2;
    else
        val -= 2;

    /* Reset the state of the vertical scale.  This provokes a "value_changed" event. */
    gtk_range_set_value (GTK_RANGE (vol->volume_scale), CLAMP((int) val, 0, 100));
}

/* Handler for "toggled" signal on popup window mute checkbox. */
static void volumealsa_popup_mute_toggled (GtkWidget *widget, VolumeALSAPlugin *vol)
{
    /* Reflect the mute toggle to the sound system. */
    asound_set_mute (vol, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)));

    /* Redraw the controls. */
    volumealsa_update_display (vol);
}

/* Build the window that appears when the top level widget is clicked. */
static void volumealsa_build_popup_window (GtkWidget *p)
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
    gtk_window_set_decorated (GTK_WINDOW (vol->popup_window), FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window), 5);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_skip_pager_hint (GTK_WINDOW (vol->popup_window), TRUE);
    gtk_window_set_type_hint (GTK_WINDOW (vol->popup_window), GDK_WINDOW_TYPE_HINT_UTILITY);

    /* Create a scrolled window as the child of the top level window. */
    GtkWidget * scrolledwindow = gtk_scrolled_window_new (NULL, NULL);
    gtk_widget_set_name (scrolledwindow, "whitewd");
    gtk_container_set_border_width (GTK_CONTAINER (scrolledwindow), 0);
    gtk_widget_show (scrolledwindow);
    gtk_container_add (GTK_CONTAINER (vol->popup_window), scrolledwindow);
    gtk_widget_set_can_focus (scrolledwindow, FALSE);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_SHADOW_NONE);

    /* Create a viewport as the child of the scrolled window. */
    GtkWidget * viewport = gtk_viewport_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scrolledwindow), viewport);
    gtk_viewport_set_shadow_type (GTK_VIEWPORT (viewport), GTK_SHADOW_NONE);
    gtk_widget_show (viewport);

    gtk_container_set_border_width (GTK_CONTAINER (vol->popup_window), 0);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolledwindow), GTK_SHADOW_IN);
    /* Create a vertical box as the child of the viewport. */
    GtkWidget * box = gtk_vbox_new (FALSE, 0);
    gtk_container_add (GTK_CONTAINER (viewport), box);

    /* Create a vertical scale as the child of the vertical box. */
    vol->volume_scale = gtk_vscale_new (GTK_ADJUSTMENT (gtk_adjustment_new (100, 0, 100, 0, 0, 0)));
    gtk_widget_set_name (vol->volume_scale, "volscale");
    g_object_set (vol->volume_scale, "height-request", 120, NULL);
    gtk_scale_set_draw_value (GTK_SCALE (vol->volume_scale), FALSE);
    gtk_range_set_inverted (GTK_RANGE (vol->volume_scale), TRUE);
    gtk_box_pack_start (GTK_BOX(box), vol->volume_scale, TRUE, TRUE, 0);
    gtk_widget_set_can_focus (vol->volume_scale, FALSE);

    /* Value-changed and scroll-event signals. */
    vol->volume_scale_handler = g_signal_connect (vol->volume_scale, "value-changed", G_CALLBACK (volumealsa_popup_scale_changed), vol);
    g_signal_connect (vol->volume_scale, "scroll-event", G_CALLBACK (volumealsa_popup_scale_scrolled), vol);

    /* Create a check button as the child of the vertical box. */
    vol->mute_check = gtk_check_button_new_with_label (_("Mute"));
    gtk_box_pack_end (GTK_BOX (box), vol->mute_check, FALSE, FALSE, 0);
    vol->mute_check_handler = g_signal_connect (vol->mute_check, "toggled", G_CALLBACK (volumealsa_popup_mute_toggled), vol);
    gtk_widget_set_can_focus (vol->mute_check, FALSE);

    /* Lock the controls if there is nothing to control... */
    gboolean def_good = FALSE;

    if (asound_bt_is_default ()) def_good = TRUE;
    else
    {
        int num = -1;
        while (1)
        {
            if (snd_card_next (&num) < 0)
            {
                g_warning ("volumealsa: Cannot enumerate devices");
                break;
            }
            if (num == -1) break;

            if (asound_is_default_card (num))
            {
                def_good = TRUE;
                break;
            }
        }
    }
    if (!def_good)
    {
        gtk_widget_set_sensitive (vol->volume_scale, FALSE);
        gtk_widget_set_sensitive (vol->mute_check, FALSE);
    }
}

/* Plugin constructor. */
static GtkWidget *volumealsa_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context and set into Plugin private data pointer. */
    VolumeALSAPlugin * vol = g_new0(VolumeALSAPlugin, 1);
    GtkWidget *p;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    vol->bt_conname = NULL;
    vol->master_element = NULL;

    /* Allocate top level widget and set into Plugin widget pointer. */
    vol->panel = panel;
    vol->plugin = p = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (vol->plugin), GTK_RELIEF_NONE);
    g_signal_connect (vol->plugin, "button-press-event", G_CALLBACK (volumealsa_button_press_event), vol->panel);
    vol->settings = settings;
    lxpanel_plugin_set_data (p, vol, volumealsa_destructor);
    gtk_widget_add_events (p, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_tooltip_text (p, _("Volume control"));

    /* Allocate icon as a child of top level. */
    vol->tray_icon = gtk_image_new();
    gtk_container_add (GTK_CONTAINER (p), vol->tray_icon);

    /* Initialize ALSA if default device isn't Bluetooth */
    if (!asound_bt_is_default ()) asound_initialize (vol);

    // set up callbacks to see if BlueZ is on DBus
    g_bus_watch_name (G_BUS_TYPE_SYSTEM, "org.bluez", 0, bt_cb_name_owned, bt_cb_name_unowned, vol, NULL);

    /* Initialize window to appear when icon clicked. */
    volumealsa_build_popup_window (p);

    /* Connect signals. */
    g_signal_connect (G_OBJECT (p), "scroll-event", G_CALLBACK (volumealsa_popup_scale_scrolled), vol);
    g_signal_connect (panel_get_icon_theme (panel), "changed", G_CALLBACK (volumealsa_theme_change), vol);

    /* Set up for multiple HDMIs */
    vol->hdmis = n_desktops (vol);

    /* Update the display, show the widget, and return. */
    volumealsa_update_display (vol);
    gtk_widget_show_all (p);

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

#ifdef ENABLE_NLS
    textdomain ( GETTEXT_PACKAGE );
#endif
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

    volumealsa_build_popup_window (vol->plugin);
    /* Do a full redraw. */
    volumealsa_update_display(vol);
    if (vol->show_popup) gtk_widget_show_all (vol->popup_window);
}

static gboolean volumealsa_control_msg (GtkWidget *plugin, const char *cmd)
{
    VolumeALSAPlugin *vol = lxpanel_plugin_get_data (plugin);

    if (!strncmp (cmd, "star", 4))
    {
        asound_initialize (vol);
        g_warning ("volumealsa: Restarted ALSA interface...");
        volumealsa_update_display (vol);
        if (vol->menu_popup) gtk_menu_popdown (GTK_MENU (vol->menu_popup));
        vol->stopped = FALSE;
        return TRUE;
    }

    if (!strncmp (cmd, "stop", 4))
    {
        asound_deinitialize (vol);
        g_warning ("volumealsa: Stopped ALSA interface...");
        volumealsa_update_display (vol);
        if (vol->menu_popup) gtk_menu_popdown (GTK_MENU (vol->menu_popup));
        vol->stopped = TRUE;
        return TRUE;
    }

    if (!strncmp (cmd, "reco", 4))
    {
        asound_restart (vol);
        volumealsa_build_popup_window (vol->plugin);
        volumealsa_update_display(vol);
        if (vol->show_popup) gtk_widget_show_all (vol->popup_window);
        return TRUE;
    }

    if (!strncmp (cmd, "mute", 4))
    {
        asound_set_mute (vol, asound_is_muted (vol) ? 0 : 1);
        volumealsa_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "volu", 4))
    {
        if (asound_is_muted (vol)) asound_set_mute (vol, 0);
        else
        {
            int volume = asound_get_volume (vol);
            if (volume < 100)
            {
                volume += 5;
                volume /= 5;
                volume *= 5;
            }
            asound_set_volume (vol, volume);
        }
        volumealsa_update_display (vol);
        return TRUE;
    }

    if (!strncmp (cmd, "vold", 4))
    {
        if (asound_is_muted (vol)) asound_set_mute (vol, 0);
        else
        {
            int volume = asound_get_volume (vol);
            if (volume > 0)
            {
                volume -= 1; // effectively -5 + 4 for rounding...
                volume /= 5;
                volume *= 5;
            }
            asound_set_volume (vol, volume);
        }
        volumealsa_update_display (vol);
        return TRUE;
    }

    return FALSE;
}

FM_DEFINE_MODULE(lxpanel_gtk, volumealsabt)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Volume Control (ALSA/BT)"),
    .description = N_("Display and control volume for ALSA and Bluetooth devices"),

    .new_instance = volumealsa_constructor,
    .config = volumealsa_configure,
    .reconfigure = volumealsa_panel_configuration_changed,
    .control = volumealsa_control_msg,
    .gettext_package = GETTEXT_PACKAGE
};

/* vim: set sw=4 et sts=4 : */
