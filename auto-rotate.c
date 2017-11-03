/*
 * Copyright (c) 2015 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 * Adjusted for auto-rotate functionality on Yoga 900 by Anne van Rossum.
 */
#include <string.h>
#include <gio/gio.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

typedef struct Matrix {
    float m[9];
} Matrix;

static void matrix_set(Matrix *m, int row, int col, float val)
{
    m->m[row * 3 + col] = val;
}

static void matrix_set_unity(Matrix *m)
{
    memset(m, 0, sizeof(m->m));
    matrix_set(m, 0, 0, 1);
    matrix_set(m, 1, 1, 1);
    matrix_set(m, 2, 2, 1);
}

static GMainLoop *loop;
static guint watch_id;
static GDBusProxy *iio_proxy, *iio_proxy_compass;

static int verbose = 0;

static int
apply_matrix(Display *dpy, int deviceid, Matrix *m)
{
    Atom prop_float, prop_matrix;

    union {
        unsigned char *c;
        float *f;
    } data;
    int format_return;
    Atom type_return;
    unsigned long nitems;
    unsigned long bytes_after;

    int rc;

    prop_float = XInternAtom(dpy, "FLOAT", False);
    prop_matrix = XInternAtom(dpy, "Coordinate Transformation Matrix", False);

    if (!prop_float)
    {
        g_print("Float atom not found. This server is too old.\n");
        return 1;
    }
    if (!prop_matrix)
    {
        g_print("Coordinate transformation matrix not found. This "
                "server is too old\n");
        return 1;
    }

    rc = XIGetProperty(dpy, deviceid, prop_matrix, 0, 9, False, prop_float,
                       &type_return, &format_return, &nitems, &bytes_after,
                       &data.c);
    if (rc != Success || prop_float != type_return || format_return != 32 ||
        nitems != 9 || bytes_after != 0)
    {
				g_print("Failed to retrieve current property values\n");
        return 1;
    }

    memcpy(data.f, m->m, sizeof(m->m));

    XIChangeProperty(dpy, deviceid, prop_matrix, prop_float,
                     format_return, PropModeReplace, data.c, nitems);

    XFree(data.c);

    return 0;
}

static void
matrix_s4(Matrix *m, float x02, float x12, float d1, float d2, int main_diag)
{
    matrix_set(m, 0, 2, x02);
    matrix_set(m, 1, 2, x12);

    if (main_diag) {
        matrix_set(m, 0, 0, d1);
        matrix_set(m, 1, 1, d2);
    } else {
        matrix_set(m, 0, 0, 0);
        matrix_set(m, 1, 1, 0);
        matrix_set(m, 0, 1, d1);
        matrix_set(m, 1, 0, d2);
    }
}


#define RR_Reflect_All	(RR_Reflect_X|RR_Reflect_Y)

static void
set_transformation_matrix(Display *dpy, Matrix *m, int offset_x, int offset_y,
                          int screen_width, int screen_height,
                          int rotation)
{
    /* total display size */
    int width = DisplayWidth(dpy, DefaultScreen(dpy));
    int height = DisplayHeight(dpy, DefaultScreen(dpy));

    /* offset */
    float x = 1.0 * offset_x/width;
    float y = 1.0 * offset_y/height;

    /* mapping */
    float w = 1.0 * screen_width/width;
    float h = 1.0 * screen_height/height;

    matrix_set_unity(m);

    /*
     * There are 16 cases:
     * Rotation X Reflection
     * Rotation: 0 | 90 | 180 | 270
     * Reflection: None | X | Y | XY
     *
     * They are spelled out instead of doing matrix multiplication to avoid
     * any floating point errors.
     */
    switch (rotation) {
    case RR_Rotate_0:
    case RR_Rotate_180 | RR_Reflect_All:
        matrix_s4(m, x, y, w, h, 1);
        break;
    case RR_Reflect_X|RR_Rotate_0:
    case RR_Reflect_Y|RR_Rotate_180:
        matrix_s4(m, x + w, y, -w, h, 1);
        break;
    case RR_Reflect_Y|RR_Rotate_0:
    case RR_Reflect_X|RR_Rotate_180:
        matrix_s4(m, x, y + h, w, -h, 1);
        break;
    case RR_Rotate_90:
    case RR_Rotate_270 | RR_Reflect_All: /* left limited - correct in working zone. */
        matrix_s4(m, x + w, y, -w, h, 0);
        break;
    case RR_Rotate_270:
    case RR_Rotate_90 | RR_Reflect_All: /* left limited - correct in working zone. */
        matrix_s4(m, x, y + h, w, -h, 0);
        break;
    case RR_Rotate_90 | RR_Reflect_X: /* left limited - correct in working zone. */
    case RR_Rotate_270 | RR_Reflect_Y: /* left limited - correct in working zone. */
        matrix_s4(m, x, y, w, h, 0);
        break;
    case RR_Rotate_90 | RR_Reflect_Y: /* right limited - correct in working zone. */
    case RR_Rotate_270 | RR_Reflect_X: /* right limited - correct in working zone. */
        matrix_s4(m, x + w, y + h, -w, -h, 0);
        break;
    case RR_Rotate_180:
    case RR_Reflect_All|RR_Rotate_0:
        matrix_s4(m, x + w, y + h, -w, -h, 1);
        break;
    }

#if DEBUG
    matrix_print(m);
#endif
}

/* Caller must free return value */
static XRROutputInfo*
find_output_xrandr(Display *dpy)
{
    XRRScreenResources *res;
    XRROutputInfo *output_info = NULL;
    int i;
    int found = 0;

    res = XRRGetScreenResources(dpy, DefaultRootWindow(dpy));

    for (i = 0; i < res->noutput && !found; i++)
    {
        output_info = XRRGetOutputInfo(dpy, res, res->outputs[i]);

        if (output_info->crtc && output_info->connection == RR_Connected)
				{
            found = 1;
            break;
        }

        XRRFreeOutputInfo(output_info);
    }

    XRRFreeScreenResources(res);

    if (!found)
        output_info = NULL;

    return output_info;
}

static int
map_output_xrandr(Display *dpy, int deviceid)
{
    int rc = EXIT_FAILURE;
    XRRScreenResources *res;
    XRROutputInfo *output_info;

    res = XRRGetScreenResources(dpy, DefaultRootWindow(dpy));
    output_info = find_output_xrandr(dpy);

    /* crtc holds our screen info, need to compare to actual screen size */
    if (output_info)
    {
        XRRCrtcInfo *crtc_info;
        Matrix m;
        matrix_set_unity(&m);
        crtc_info = XRRGetCrtcInfo (dpy, res, output_info->crtc);
        set_transformation_matrix(dpy, &m, crtc_info->x, crtc_info->y,
                                  crtc_info->width, crtc_info->height,
                                  crtc_info->rotation);
        rc = apply_matrix(dpy, deviceid, &m);
        XRRFreeCrtcInfo(crtc_info);
        XRRFreeOutputInfo(output_info);
    } else
        g_print("Unable to find output "
                "Output may not be connected.\n");

    XRRFreeScreenResources(res);

    return rc;
}
Bool is_pointer(int use)
{
    return use == XIMasterPointer || use == XISlavePointer;
}

Bool is_keyboard(int use)
{
    return use == XIMasterKeyboard || use == XISlaveKeyboard;
}

Bool device_matches(XIDeviceInfo *info)
{
    if (is_pointer(info->use)) {
        return True;
    }

    return False;
}

static void
properties_changed (GDBusProxy *proxy,
		    GVariant   *changed_properties,
		    GStrv       invalidated_properties,
		    gpointer    user_data)
{
	GVariant *v;
	GVariantDict dict;

	g_variant_dict_init (&dict, changed_properties);

	if (g_variant_dict_contains (&dict, "AccelerometerOrientation")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "AccelerometerOrientation");

		Rotation new_rotation;

		char recognized = 1;

		if (g_strcmp0 ( g_variant_get_string (v, NULL), "normal") == 0) {
			new_rotation = RR_Rotate_0;			
		} else if (g_strcmp0 ( g_variant_get_string (v, NULL), "left-up") == 0) {
			new_rotation = RR_Rotate_90;			
		} else if (g_strcmp0 ( g_variant_get_string (v, NULL), "bottom-up") == 0) {
			new_rotation = RR_Rotate_180;			
		} else if (g_strcmp0 ( g_variant_get_string (v, NULL), "right-up") == 0) {
			new_rotation = RR_Rotate_270;			
		} else {
			recognized = 0;
		}
			
		if (recognized) {
			XRROutputChangeNotifyEvent xrrchange;
			XGravityEvent xgrav;

			Display *dpy;
			Window root;
			int screen;
			XRRScreenConfiguration * screen_config;
			Rotation rotation, current_rotation;
			SizeID current_size;

			dpy = XOpenDisplay (NULL); 
			if (!dpy) { 
				g_print("Error with XOpenDisplay\n");
				return;
			}
			if (dpy)
			{
				screen = DefaultScreen(dpy);
				root = DefaultRootWindow(dpy); 
				screen_config = XRRGetScreenInfo(dpy, root);
				if ( screen_config == NULL ) {
					g_print ( "Cannot get screen info\n" ) ;
					return;
				}
			}

			rotation = XRRRotations (dpy, screen, &current_rotation);
			current_size = XRRConfigCurrentConfiguration (screen_config, &current_rotation);

			if (verbose > 0) {
				g_print(" %d	.. XGravityEvent X Origin\n",xgrav.x);
				g_print(" %d	.. XGravityEvent Y Origin\n",xgrav.y);
				g_print(" %s	.. Display Name\n",XDisplayName((char*)dpy));
				g_print(" %d	.. Screen Number\n",screen);
				g_print(" %d	.. XRROutputChangeNotifyEvent Rotation\n",xrrchange.rotation);
				g_print(" %x	.. XRRRotations Bitmask\n", current_rotation);
				g_print(" %d	.. XRRRotations Integer\n", current_rotation);
			}

			switch (current_rotation)
			{
				case RR_Rotate_0:
					g_print("Current X Rotation is normal\n");
					break;
				case RR_Rotate_90:
					g_print("Current X Rotation is left\n");
					break;
				case RR_Rotate_180:
					g_print("Current X Rotation is upside down\n");
					break;
				case RR_Rotate_270:
					g_print("Current X Rotation is right\n");
					break;
				default:
					g_print("Error with value of current_rotation\n");
			}

			switch (new_rotation)
			{
				case RR_Rotate_0:
					g_print("New X Rotation is normal\n");
					break;
				case RR_Rotate_90:
					g_print("New X Rotation is left\n");
					break;
				case RR_Rotate_180:
					g_print("New X Rotation is upside down\n");
					break;
				case RR_Rotate_270:
					g_print("New X Rotation is right\n");
					break;
				default:
					g_print("Error with value of new_rotation\n");
			}

			XRRSetScreenConfig(dpy, screen_config, root, current_size, new_rotation, CurrentTime);
    	XIDeviceInfo *info;
			int 	i;
			int		num_devices;
			info = XIQueryDevice(dpy, XIAllDevices, &num_devices);
			for(i = 0; i < num_devices; i++)
			{
					if(device_matches(&info[i])){
							g_print("Adjusting %s", info[i].name );
							map_output_xrandr(dpy, info[i].deviceid );
					}
			}
			XIFreeDeviceInfo(info);

		}

		g_print ("    Accelerometer orientation changed: %s\n", g_variant_get_string (v, NULL));
		g_variant_unref (v);
	}
}

static void
appeared_cb (GDBusConnection *connection,
	     const gchar     *name,
	     const gchar     *name_owner,
	     gpointer         user_data)
{
	GError *error = NULL;

	g_print ("+++ iio-sensor-proxy appeared\n");

	iio_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
						   G_DBUS_PROXY_FLAGS_NONE,
						   NULL,
						   "net.hadess.SensorProxy",
						   "/net/hadess/SensorProxy",
						   "net.hadess.SensorProxy",
						   NULL, NULL);

	g_signal_connect (G_OBJECT (iio_proxy), "g-properties-changed",
			  G_CALLBACK (properties_changed), NULL);

	/* Accelerometer */
	g_dbus_proxy_call_sync (iio_proxy,
				"ClaimAccelerometer",
				NULL,
				G_DBUS_CALL_FLAGS_NONE,
				-1,
				NULL, &error);
	g_assert_no_error (error);
}

static void
vanished_cb (GDBusConnection *connection,
	     const gchar *name,
	     gpointer user_data)
{
	if (iio_proxy || iio_proxy_compass) {
		g_clear_object (&iio_proxy);
		g_clear_object (&iio_proxy_compass);
		g_print ("--- iio-sensor-proxy vanished, waiting for it to appear\n");
	}
}

int main (int argc, char **argv)
{
	if (argc > 1) {
		if (g_strcmp0(argv[1], "--verbose") == 0) {
			verbose = 1;
		}
	}

	if (!verbose) {
		daemon(0, 0);
	}

	watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
				     "net.hadess.SensorProxy",
				     G_BUS_NAME_WATCHER_FLAGS_NONE,
				     appeared_cb,
				     vanished_cb,
				     NULL, NULL);

	g_print ("    Waiting for iio-sensor-proxy to appear\n");
	loop = g_main_loop_new (NULL, TRUE);
	g_main_loop_run (loop);

	return 0;
}
