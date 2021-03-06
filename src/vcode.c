/*	vcode.c
	Copyright (C) 2013 Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

#include "global.h"

#include "mygtk.h"
#include "memory.h"
#include "inifile.h"
#include "png.h"
#include "mainwindow.h"
#include "otherwindow.h"
#include "cpick.h"
#include "icons.h"
#include "fpick.h"
#include "vcode.h"

/* Make code not compile if it cannot work */
typedef char Opcodes_Too_Long[2 * (op_LAST <= WB_OPMASK) - 1];

/// V-CODE ENGINE

/* Max V-code subroutine nesting */
#define CALL_DEPTH 16
/* Max container widget nesting */
#define CONT_DEPTH 128
/* Max columns in a list */
#define MAX_COLS 16

#define GET_OP(S) ((int)*(void **)(S)[1] & WB_OPMASK)

enum {
	pk_NONE = 0,
	pk_PACK,
	pk_PACKp,
	pk_XPACK,
	pk_XPACK1,
	pk_PACKEND,
	pk_TABLE,
	pk_TABLEx,
	pk_TABLEp,
	pk_TABLE2,
	pk_TABLE2x,
	pk_SCROLLVP,
	pk_SCROLLVPn
};
#define pk_MASK     0xFF
#define pkf_FRAME  0x100
#define pkf_STACK  0x200
#define pkf_PARENT 0x400
#define pkf_SHOW   0x800

/* Internal datastore */

#define GET_VDATA(V) ((V)[1])

typedef struct {
	void *code;	// Noop tag, must be first field
	void ***dv;	// Pointer to dialog response
	char *ininame;	// Prefix for inifile vars
	int xywh[4];	// Stored window position & size
} v_dd;

/* From event to its originator */
void **origin_slot(void **slot)
{
	while (((int)*(void **)slot[1] & WB_OPMASK) >= op_EVT_0) slot -= 2;
	return (slot);
}

void dialog_event(void *ddata, void **wdata, int what, void **where)
{
	v_dd *vdata = GET_VDATA(wdata);

	if (((int)vdata->code & WB_OPMASK) != op_WDONE) return; // Paranoia
	if (vdata->dv) *vdata->dv = where;
}

/* !!! Warning: handlers should not access datastore after window destruction!
 * GTK+ refs objects for signal duration, but no guarantee every other toolkit
 * will behave alike - WJ */

static void get_evt_1(GtkObject *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];

	((evt_fn)desc[1])(GET_DDATA(base), base, (int)desc[0] & WB_OPMASK, slot);
}

static void get_evt_1_t(GtkObject *widget, gpointer user_data)
{
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
		get_evt_1(widget, user_data);
}

static void get_evt_1_d(GtkObject *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];

	if (!desc[1]) run_destroy(base);
	else ((evt_fn)desc[1])(GET_DDATA(base), base, (int)desc[0] & WB_OPMASK, slot);
}

static void add_click(void **r, void **pp, GtkWidget *widget, int destroy)
{
	if (pp[1] || destroy) gtk_signal_connect(GTK_OBJECT(widget), "clicked",
		GTK_SIGNAL_FUNC(get_evt_1_d), r);
}

static void **skip_if(void **pp)
{
	int lp, mk;
	void **ifcode;

	ifcode = pp + 1 + (lp = (int)*pp >> 16);
	if (lp > 1) // skip till corresponding ENDIF
	{
		mk = (int)pp[2];
		while ((((int)*ifcode & WB_OPMASK) != op_ENDIF) ||
			((int)ifcode[1] != mk))
			ifcode += 1 + ((int)*ifcode >> 16);
	}
	return (ifcode + 1 + ((int)*ifcode >> 16));
}

/* Trigger events which need triggering */
static void trigger_things(void **wdata)
{
	char *data = GET_DDATA(wdata);
	void **slot, **desc;

	for (wdata = GET_WINDOW(wdata); wdata[1]; wdata += 2)
	{
		if (GET_OP(wdata) != op_TRIGGER) continue;
		slot = wdata - 2;
		desc = slot[1];
		((evt_fn)desc[1])(data, slot[0], (int)desc[0] & WB_OPMASK, slot);
	}
}

/* Predict how many _slots_ a V-code sequence could need */
// !!! With GCC inlining this, weird size fluctuations can happen
static int predict_size(void **ifcode, char *ddata)
{
	void **v, **pp, *rstack[CALL_DEPTH], **rp = rstack;
	int op, n = 2; // safety margin

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + (op >> 16);
		n += WB_GETREF(op);
		op &= WB_OPMASK;
		if (op < op_END_LAST) break; // End
		// Subroutine call/return
		if (op == op_RET) ifcode = *--rp;
		else if (op == op_CALLp)
		{
			*rp++ = ifcode;
			v = *pp;
			if ((int)*(pp - 1) & WB_FFLAG)
				v = (void *)(ddata + (int)v);
			ifcode = *v;
		}
	}
	return (n);
}

// !!! And with inlining this, same problem
void table_it(GtkWidget *table, GtkWidget *it, int wh, int pad, int pack)
{
	int row = wh & 255, column = (wh >> 8) & 255, l = (wh >> 16) + 1;
	gtk_table_attach(GTK_TABLE(table), it, column, column + l, row, row + 1,
		pack == pk_TABLEx ? GTK_EXPAND | GTK_FILL : GTK_FILL, 0,
		pack == pk_TABLEp ? pad : 0, pad);
}

/* Find where unused rows/columns start */
static int next_table_level(GtkWidget *table, int h)
{
	GList *item;
	int y, n = 0;
	for (item = GTK_TABLE(table)->children; item; item = item->next)
	{
		y = h ? ((GtkTableChild *)item->data)->right_attach :
			((GtkTableChild *)item->data)->bottom_attach;
		if (n < y) n = y;
	}
	return (n);
}

/* Try to avoid scrolling - request full size of contents */
static void scroll_max_size_req(GtkWidget *widget, GtkRequisition *requisition,
	gpointer user_data)
{
	GtkWidget *child = GTK_BIN(widget)->child;

	if (child && GTK_WIDGET_VISIBLE(child))
	{
		GtkRequisition wreq;
		int n, border = GTK_CONTAINER(widget)->border_width * 2;

		gtk_widget_get_child_requisition(child, &wreq);
		n = wreq.width + border;
		if (requisition->width < n) requisition->width = n;
		n = wreq.height + border;
		if (requisition->height < n) requisition->height = n;
	}
}

/* Toggle notebook pages */
static void toggle_vbook(GtkToggleButton *button, gpointer user_data)
{
	gtk_notebook_set_page(**(void ***)user_data,
		!!gtk_toggle_button_get_active(button));
}

//	COLORLIST widget

typedef struct {
	unsigned char *col;
	int cnt, *idx;
	int scroll;
} colorlist_data;

// !!! ref to RGB[3]
static gboolean col_expose(GtkWidget *widget, GdkEventExpose *event,
	unsigned char *col)
{
	GdkGCValues sv;

	gdk_gc_get_values(widget->style->black_gc, &sv);
	gdk_rgb_gc_set_foreground(widget->style->black_gc, MEM_2_INT(col, 0));
	gdk_draw_rectangle(widget->window, widget->style->black_gc, TRUE,
		event->area.x, event->area.y, event->area.width, event->area.height);
	gdk_gc_set_foreground(widget->style->black_gc, &sv.foreground);

	return (TRUE);
}

static gboolean colorlist_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_ext xdata;

	if (event->type == GDK_BUTTON_PRESS)
	{
		xdata.idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
		xdata.button = event->button;
		((evtx_fn)desc[1])(GET_DDATA(base), base,
			(int)desc[0] & WB_OPMASK, slot, &xdata);
	}

	/* Let click processing continue */
	return (FALSE);
}

static void colorlist_select(GtkList *list, GtkWidget *widget, gpointer user_data)
{
	void **slot = user_data;
	void **base = slot[0], **desc = slot[1];
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));

	/* Update the value */
	*dt->idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
}

static void colorlist_map_scroll(GtkWidget *list, colorlist_data *dt)
{	
	GtkAdjustment *adj;
	int idx = dt->scroll - 1;

	dt->scroll = 0;
	if (idx < 0) return;
	adj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(list->parent->parent));
	if (adj->upper > adj->page_size)
	{
		float f = adj->upper * (idx + 0.5) / dt->cnt - adj->page_size * 0.5;
		adj->value = f < 0.0 ? 0.0 : f > adj->upper - adj->page_size ?
			adj->upper - adj->page_size : f;
		gtk_adjustment_value_changed(adj);
	}
}

// !!! And with inlining this, problem also
GtkWidget *colorlist(GtkWidget *box, int *idx, char *ddata, void **pp,
	void **r)
{
	GtkWidget *scroll, *list, *item, *col, *label;
	colorlist_data *dt;
	void *v;
	char txt[64], *t, **sp = NULL;
	int i, cnt = 0;

	scroll = pack(box, gtk_scrolled_window_new(NULL, NULL));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	list = gtk_list_new();

	// Allocate datablock
	dt = bound_malloc(list, sizeof(colorlist_data));
	v = ddata + (int)pp[3];
	if (((int)pp[0] & WB_OPMASK) == op_COLORLIST) // array of names
	{
		sp = *(char ***)v;
		while (sp[cnt]) cnt++;
	}
	else cnt = *(int *)v; // op_COLORLISTN - number
	dt->cnt = cnt;
	dt->col = (void *)(ddata + (int)pp[2]); // palette
	dt->idx = idx;

	gtk_object_set_user_data(GTK_OBJECT(list), dt); // know thy descriptor
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), list);
	gtk_widget_show_all(scroll);

	for (i = 0; i < cnt; i++)
	{
		item = gtk_list_item_new();
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		if (pp[5]) gtk_signal_connect(GTK_OBJECT(item), "button_press_event",
			GTK_SIGNAL_FUNC(colorlist_click), r);
		gtk_container_add(GTK_CONTAINER(list), item);

		box = gtk_hbox_new(FALSE, 3);
		gtk_widget_show(box);
		gtk_container_set_border_width(GTK_CONTAINER(box), 3);
		gtk_container_add(GTK_CONTAINER(item), box);

		col = pack(box, gtk_drawing_area_new());
		gtk_drawing_area_size(GTK_DRAWING_AREA(col), 20, 20);
		gtk_signal_connect(GTK_OBJECT(col), "expose_event",
			GTK_SIGNAL_FUNC(col_expose), dt->col + i * 3);

		/* Name or index */
		if (sp) t = _(sp[i]);
		else sprintf(t = txt, "%d", i);
		label = xpack(box, gtk_label_new(t));
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 1.0);

		gtk_widget_show_all(item);
	}
	gtk_list_set_selection_mode(GTK_LIST(list), GTK_SELECTION_BROWSE);
	/* gtk_list_select_*() don't work in GTK_SELECTION_BROWSE mode */
	gtk_container_set_focus_child(GTK_CONTAINER(list),
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, *idx)->data));
	gtk_signal_connect(GTK_OBJECT(list), "select_child",
		GTK_SIGNAL_FUNC(colorlist_select), NEXT_SLOT(r));
	gtk_signal_connect_after(GTK_OBJECT(list), "map",
		GTK_SIGNAL_FUNC(colorlist_map_scroll), dt);

	return (list);
}

static void colorlist_scroll_in(GtkWidget *list, int idx)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	dt->scroll = idx + 1;
	if (GTK_WIDGET_MAPPED(list)) colorlist_map_scroll(list, dt);
}

static void colorlist_set_color(GtkWidget *list, int idx, int v)
{
	colorlist_data *dt = gtk_object_get_user_data(GTK_OBJECT(list));
	unsigned char *rgb = dt->col + idx * 3;
	GdkColor c;

	c.pixel = 0;
	c.red   = (rgb[0] = INT_2_R(v)) * 257;
	c.green = (rgb[1] = INT_2_G(v)) * 257;
	c.blue  = (rgb[2] = INT_2_B(v)) * 257;
	// In case of some really ancient system with indexed display mode
	gdk_colormap_alloc_color(gdk_colormap_get_system(), &c, FALSE, TRUE);
	/* Redraw the item displaying the color */
	gtk_widget_queue_draw(
		GTK_WIDGET(g_list_nth(GTK_LIST(list)->children, idx)->data));
}

//	COLORPAD widget

#define PPAD_SLOT 11
#define PPAD_XSZ 32
#define PPAD_YSZ 8
#define PPAD_WIDTH(X) (PPAD_XSZ * (X) - 1)
#define PPAD_HEIGHT(X) (PPAD_YSZ * (X) - 1)

static void colorpad_set(void **slot, int v)
{
	GtkWidget *widget = slot[0];
	void **desc = slot[1];

	wjpixmap_move_cursor(widget, (v % PPAD_XSZ) * PPAD_SLOT,
		(v / PPAD_XSZ) * PPAD_SLOT);
	*(int *)gtk_object_get_user_data(GTK_OBJECT(widget)) = v; // self-reading
	if (desc[4]) get_evt_1(NULL, NEXT_SLOT(slot)); // call handler
}

static gboolean colorpad_click(GtkWidget *widget, GdkEventButton *event,
	gpointer user_data)
{
	int x = event->x, y = event->y;

	gtk_widget_grab_focus(widget);
	/* Only single clicks */
	if (event->type != GDK_BUTTON_PRESS) return (TRUE);
	x /= PPAD_SLOT; y /= PPAD_SLOT;
	colorpad_set(user_data, y * PPAD_XSZ + x);
	return (TRUE);
}

static gboolean colorpad_key(GtkWidget *widget, GdkEventKey *event,
	gpointer user_data)
{
	int x, y, dx, dy;

	if (!arrow_key(event, &dx, &dy, 0)) return (FALSE);
	wjpixmap_cursor(widget, &x, &y);
	x = x / PPAD_SLOT + dx; y = y / PPAD_SLOT + dy;
	y = y < 0 ? 0 : y >= PPAD_YSZ ? PPAD_YSZ - 1 : y;
	y = y * PPAD_XSZ + x;
	y = y < 0 ? 0 : y > 255 ? 255 : y;
	colorpad_set(user_data, y);
#if GTK_MAJOR_VERSION == 1
	/* Return value alone doesn't stop GTK1 from running other handlers */
	gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
#endif
	return (TRUE);
}

static void colorpad_draw(GtkWidget *widget, gpointer user_data)
{
	unsigned char *rgb;
	int w, h, c;

	if (!wjpixmap_pixmap(widget)) return;
	w = PPAD_WIDTH(PPAD_SLOT);
	h = PPAD_HEIGHT(PPAD_SLOT);
	rgb = render_color_grid(w, h, PPAD_SLOT, user_data);
	if (!rgb) return;
	wjpixmap_draw_rgb(widget, 0, 0, w, h, rgb, w * 3);
	c = (PPAD_SLOT >> 1) - 1;
	wjpixmap_set_cursor(widget, xbm_ring4_bits, xbm_ring4_mask_bits,
		xbm_ring4_width, xbm_ring4_height,
		xbm_ring4_x_hot - c, xbm_ring4_y_hot - c, TRUE);
	free(rgb);
}

static GtkWidget *colorpad(int *idx, char *ddata, void **pp, void **r)
{
	GtkWidget *pix;

	pix = wjpixmap_new(PPAD_WIDTH(PPAD_SLOT), PPAD_HEIGHT(PPAD_SLOT));
	gtk_object_set_user_data(GTK_OBJECT(pix), (gpointer)idx);
	gtk_signal_connect(GTK_OBJECT(pix), "realize",
		GTK_SIGNAL_FUNC(colorpad_draw), (gpointer)(ddata + (int)pp[2]));
	r -= 2; // using the origin slot
	gtk_signal_connect(GTK_OBJECT(pix), "button_press_event",
		GTK_SIGNAL_FUNC(colorpad_click), (gpointer)r);
	gtk_signal_connect(GTK_OBJECT(pix), "key_press_event",
		GTK_SIGNAL_FUNC(colorpad_key), (gpointer)r);
	return (pix);
}

//	GRADBAR widget

#define GRADBAR_LEN 16
#define SLOT_SIZE 15

typedef struct {
	unsigned char *map, *rgb;
	GtkWidget *lr[2];
	void **r;
	int ofs, lim, *idx, *len;
} gradbar_data;

static void gradbar_scroll(GtkWidget *btn, gpointer user_data)
{
	gradbar_data *dt = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	int dir = (int)user_data * 2 - 1;

	dt->ofs += dir;
	*dt->idx += dir; // self-reading
	gtk_widget_set_sensitive(dt->lr[0], !!dt->ofs);
	gtk_widget_set_sensitive(dt->lr[1], dt->ofs < dt->lim - GRADBAR_LEN);
	gtk_widget_queue_draw(btn->parent);
	get_evt_1(NULL, (gpointer)dt->r);
}

static void gradbar_slot(GtkWidget *btn, gpointer user_data)
{
	gradbar_data *dt;

	if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn))) return;
	dt = gtk_object_get_user_data(GTK_OBJECT(btn->parent));
	*dt->idx = (int)user_data + dt->ofs; // self-reading
	get_evt_1(NULL, (gpointer)dt->r);
}

static gboolean gradbar_draw(GtkWidget *widget, GdkEventExpose *event,
	gpointer idx)
{
	unsigned char rgb[SLOT_SIZE * 2 * 3];
	gradbar_data *dt = gtk_object_get_user_data(
		GTK_OBJECT(widget->parent->parent));
	int i, n = (int)idx + dt->ofs;

	if (n < *dt->len) // Filled slot
	{
		memcpy(rgb, dt->rgb ? dt->rgb + dt->map[n] * 3 :
			dt->map + n * 3, 3);
		for (i = 3; i < SLOT_SIZE * 2 * 3; i++) rgb[i] = rgb[i - 3];
	}
	else // Empty slot - show that
	{
		memset(rgb, 178, sizeof(rgb));
		memset(rgb, 128, SLOT_SIZE * 3);
	}

	gdk_draw_rgb_image(widget->window, widget->style->black_gc,
		0, 0, SLOT_SIZE, SLOT_SIZE, GDK_RGB_DITHER_NONE,
		rgb + SLOT_SIZE * 3, -3);

	return (TRUE);
}

// !!! With inlining this, problem also
GtkWidget *gradbar(int *idx, char *ddata, void **pp, void **r)
{
	GtkWidget *hbox, *btn, *sw;
	gradbar_data *dt;
	int i;

	hbox = gtk_hbox_new(TRUE, 0);
	dt = bound_malloc(hbox, sizeof(gradbar_data));
	gtk_object_set_user_data(GTK_OBJECT(hbox), dt);

	dt->r = r;
	dt->idx = idx;
	dt->len = (void *)(ddata + (int)pp[3]); // length
	dt->map = (void *)(ddata + (int)pp[4]); // gradient map
	if (*(int *)(ddata + (int)pp[2])) // mode not-RGB
		dt->rgb = (void *)(ddata + (int)pp[5]); // colormap
	dt->lim = (int)pp[6];

	dt->lr[0] = btn = xpack(hbox, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_LEFT,
		GTK_SHADOW_NONE));
	gtk_widget_set_sensitive(btn, FALSE);
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(gradbar_scroll), (gpointer)0);
	btn = NULL;
	for (i = 0; i < GRADBAR_LEN; i++)
	{
		btn = xpack(hbox, gtk_radio_button_new_from_widget(
			GTK_RADIO_BUTTON_0(btn)));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_signal_connect(GTK_OBJECT(btn), "toggled",
			GTK_SIGNAL_FUNC(gradbar_slot), (gpointer)i);
		sw = gtk_drawing_area_new();
		gtk_container_add(GTK_CONTAINER(btn), sw);
		gtk_widget_set_usize(sw, SLOT_SIZE, SLOT_SIZE);
		gtk_signal_connect(GTK_OBJECT(sw), "expose_event",
			GTK_SIGNAL_FUNC(gradbar_draw), (gpointer)i);
	}
	dt->lr[1] = btn = xpack(hbox, gtk_button_new());
	gtk_container_add(GTK_CONTAINER(btn), gtk_arrow_new(GTK_ARROW_RIGHT,
		GTK_SHADOW_NONE));
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
		GTK_SIGNAL_FUNC(gradbar_scroll), (gpointer)1);

	gtk_widget_show_all(hbox);
	return (hbox);
}

//	TEXT widget

static GtkWidget *textarea(char *init)
{
	GtkWidget *scroll, *text;

#if GTK_MAJOR_VERSION == 1
	text = gtk_text_new(NULL, NULL);
	gtk_text_set_editable(GTK_TEXT(text), TRUE);
	if (init) gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, init, -1);

	scroll = gtk_scrolled_window_new(NULL, GTK_TEXT(text)->vadj);
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkTextBuffer *texbuf = gtk_text_buffer_new(NULL);
	if (init) gtk_text_buffer_set_text(texbuf, init, -1);

	text = gtk_text_view_new_with_buffer(texbuf);

	scroll = gtk_scrolled_window_new(GTK_TEXT_VIEW(text)->hadjustment,
		GTK_TEXT_VIEW(text)->vadjustment);
#endif
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), text);
	gtk_widget_show_all(scroll);

	return (text);
}

static char *read_textarea(GtkWidget *text)
{
#if GTK_MAJOR_VERSION == 1
	return (gtk_editable_get_chars(GTK_EDITABLE(text), 0, -1));
#else /* #if GTK_MAJOR_VERSION == 2 */
	GtkTextIter begin, end;
	GtkTextBuffer *buffer = GTK_TEXT_VIEW(text)->buffer;

	gtk_text_buffer_get_start_iter(buffer, &begin);
	gtk_text_buffer_get_end_iter(buffer, &end);
	return (gtk_text_buffer_get_text(buffer, &begin, &end, TRUE));
#endif
}

static void set_textarea(GtkWidget *text, char *init)
{
#if GTK_MAJOR_VERSION == 1
	gtk_editable_delete_text(GTK_EDITABLE(text), 0, -1);
	if (init) gtk_text_insert(GTK_TEXT(text), NULL, NULL, NULL, init, -1);
#else /* #if GTK_MAJOR_VERSION == 2 */
	gtk_text_buffer_set_text(GTK_TEXT_VIEW(text)->buffer, init ? init : "", -1);
#endif
}

//	LISTCC widget

typedef struct {
	void **r;	// slot
	unsigned char idx[MAX_COLS]; // index vector
} ref_data;

typedef struct {
	int cnt, bk, ncol;	// height, direction, columns
	int *idx;		// result field
	void **columns[MAX_COLS]; // column slots
	ref_data ref;		// own slot, index ref targets
} listcc_data;

// !!! Won't work when the list is insensitive
static void listcc_select_item(GtkWidget *list, int n)
{
	GList *slot = g_list_nth(GTK_LIST(list)->children, n);
	if (slot) list_select_item(list, GTK_WIDGET(slot->data));
}

static void listcc_select(GtkList *list, GtkWidget *widget, gpointer user_data)
{
	listcc_data *dt = user_data;
	void **slot = NEXT_SLOT(dt->ref.r), **base = slot[0], **desc = slot[1];
	int idx = (int)gtk_object_get_user_data(GTK_OBJECT(widget));

	/* Update the value */
	if (dt->bk) idx = dt->cnt - idx - 1; // Reverse indices
	*dt->idx = idx;
	/* Call the handler */
	if (desc[1]) ((evt_fn)desc[1])(GET_DDATA(base), base,
		(int)desc[0] & WB_OPMASK, slot);
}

// !!! With inlining this, problem also
GtkWidget *listcc(int *idx, char *ddata, void **pp, void ***columns,
	int ncol, void **r)
{
	char *str, txt[128];
	GtkWidget *label, *list, *item, *hbox;
	listcc_data *ld;
	int i, j, n, cnt, bk;


	cnt = *(int *)(void *)(ddata + (int)pp[2]); // length
	bk = ((int)pp[0] & WB_OPMASK) == op_LISTCCr; // bottom to top

	list = gtk_list_new();
	gtk_list_set_selection_mode(GTK_LIST(list), GTK_SELECTION_BROWSE);

	/* Make datastruct */
	ld = bound_malloc(list, sizeof(listcc_data));
	for (i = 0; i < MAX_COLS; i++) ld->ref.idx[i] = i;
	ld->ref.r = r;
	ld->idx = idx;
	ld->cnt = cnt;
	ld->bk = bk;
	ld->ncol = ncol;
	/* Link columns to list */
	for (i = 0; i < ncol; i++)
		*(ld->columns[i] = columns[i]) = ld->ref.idx + i;

	for (i = 0; i < cnt; i++)
	{
		item = gtk_list_item_new();
		gtk_object_set_user_data(GTK_OBJECT(item), (gpointer)i);
		gtk_container_add(GTK_CONTAINER(list), item);
// !!! Spacing = 3
		hbox = gtk_hbox_new(FALSE, 3);
		gtk_container_add(GTK_CONTAINER(item), hbox);
		n = bk ? cnt - i - 1 : i; // backward/forward

		for (j = 0; j < ncol; j++)
		{
			void **cp = columns[j][1];
			char *v = cp[1];
			int op = (int)cp[0], step = (int)cp[2], jw = (int)cp[3];

			if (op & WB_FFLAG) v = (void *)(ddata + (int)v);
// !!! Will need "D" group, for *v as pointer to array
			v += step * n;
			op &= WB_OPMASK;
			if (op == op_TXTCOL) str = v; // Array of chars
			else /* if (op == op_IDXCOL) */ // Constant
				sprintf(str = txt, "%d", (int)v);
			label = pack(hbox, gtk_label_new(str));
			if (jw & 0xFFFF)
				gtk_widget_set_usize(label, jw & 0xFFFF, -2);
			gtk_misc_set_alignment(GTK_MISC(label),
				(jw >> 16) * 0.5, 0.5);
		}
	}
	gtk_widget_show_all(list);

	i = *idx;
	if (bk) i = cnt - i - 1; // reverse order
	listcc_select_item(list, i);
	gtk_signal_connect(GTK_OBJECT(list), "select_child",
		GTK_SIGNAL_FUNC(listcc_select), ld);

	return (list);
}

#if U_NLS

/* Translate array of strings */
static int n_trans(char **dest, char **src, int n)
{
	int i;
	for (i = 0; (i != n) && src[i]; i++) dest[i] = _(src[i]);
	return (i);
}

#endif

/* Get/set window position & size from/to inifile */
void rw_pos(v_dd *vdata, int set)
{
	char name[128];
	int i, l = strlen(vdata->ininame);

	memcpy(name, vdata->ininame, l);
	name[l++] = '_'; name[l + 1] = '\0';
	for (i = 0; i < 4; i++)
	{
		name[l] = "xywh"[i];
		if (set) inifile_set_gint32(name, vdata->xywh[i]);
		else if (vdata->xywh[i] || (i < 2)) /* 0 means auto-size */
			vdata->xywh[i] = inifile_get_gint32(name, vdata->xywh[i]);
	}
}

/* Groups of codes differing only in details of packing */

typedef struct {
	unsigned short op, cmd, pk;	// orig code, group code, packing mode
	signed char tpad, cw;		// padding and border
} cmdef;

enum {
	cm_TABLE = op_LAST,
	cm_VBOX,
	cm_HBOX,
	cm_SPIN,
	cm_SPINa,
	cm_FSPIN,
	cm_SPINSLIDEa,
	cm_CHECK,
	cm_RPACK,
	cm_RPACKD,
	cm_OPT,
	cm_OPTD,
	cm_CANCELBTN,
	cm_BUTTON,
	cm_TOGGLE
};

#define USE_BORDER(T) (op_BOR_0 - op_BOR_##T - 1)

static cmdef cmddefs[] = {
// !!! Padding = 0
	{ op_TABLE, cm_TABLE, pk_PACK | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(TABLE) },
// !!! Padding = 0 Border = 0
	{ op_XTABLE, cm_TABLE, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_TLHBOX, cm_HBOX, pk_TABLE | pkf_STACK | pkf_SHOW },
	{ op_HBOX, cm_HBOX, pk_PACKp | pkf_STACK | pkf_SHOW },
	{ op_XHBOX, cm_HBOX, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_VBOX, cm_VBOX, pk_PACKp | pkf_STACK | pkf_SHOW },
	{ op_XVBOX, cm_VBOX, pk_XPACK | pkf_STACK | pkf_SHOW },
	{ op_EVBOX, cm_VBOX, pk_PACKEND | pkf_STACK | pkf_SHOW },
// !!! Padding = 0
	{ op_FVBOX, cm_VBOX, pk_PACK | pkf_FRAME | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(FRBOX) },
	{ op_FXVBOX, cm_VBOX, pk_XPACK | pkf_FRAME | pkf_STACK | pkf_SHOW,
		0, USE_BORDER(FRBOX) },
	/* Codes can map to themselves, and some do */
	{ op_MLABEL, op_MLABEL, pk_PACKp | pkf_SHOW, USE_BORDER(LABEL) },
// !!! Border = 5
	{ op_MLABELp, op_MLABELp, pk_PACKp | pkf_SHOW, USE_BORDER(LABEL), 5 },
	{ op_TLLABEL, op_TLLABEL, pk_TABLEp | pkf_SHOW, USE_BORDER(TLABEL) },
	{ op_SPIN, cm_SPIN, pk_PACKp, USE_BORDER(SPIN) },
// !!! Padding = 0
	{ op_XSPIN, cm_SPIN, pk_XPACK },
	{ op_TSPIN, cm_SPIN, pk_TABLE2, USE_BORDER(SPIN) },
	{ op_TLSPIN, cm_SPIN, pk_TABLE, USE_BORDER(SPIN) },
	{ op_TLXSPIN, cm_SPIN, pk_TABLEx, USE_BORDER(SPIN) },
	{ op_SPINa, cm_SPINa, pk_PACKp, USE_BORDER(SPIN) },
	{ op_FSPIN, cm_FSPIN, pk_PACKp, USE_BORDER(SPIN) },
	{ op_TFSPIN, cm_FSPIN, pk_TABLE2, USE_BORDER(SPIN) },
	{ op_TLFSPIN, cm_FSPIN, pk_TABLE, USE_BORDER(SPIN) },
// !!! Padding = 0
	{ op_XSPINa, cm_SPINa, pk_XPACK },
	{ op_TSPINa, cm_SPINa, pk_TABLE2, USE_BORDER(SPIN) },
// !!! Padding = 0
	{ op_TSPINSLIDE, op_TSPINSLIDE, pk_TABLE2x },
// !!! Padding = 5
	{ op_TLSPINSLIDE, op_TLSPINSLIDE, pk_TABLE, 5 },
	{ op_SPINSLIDEa, cm_SPINSLIDEa, pk_PACK },
	{ op_XSPINSLIDEa, cm_SPINSLIDEa, pk_XPACK },
	{ op_CHECK, cm_CHECK, pk_PACK },
	{ op_XCHECK, cm_CHECK, pk_XPACK },
	{ op_TLCHECK, cm_CHECK, pk_TABLE },
	{ op_RPACK, cm_RPACK, pk_XPACK },
	{ op_RPACKD, cm_RPACKD, pk_XPACK },
	{ op_FRPACK, cm_RPACK, pk_PACK | pkf_FRAME, 0, USE_BORDER(FRBOX) },
	{ op_OPT, cm_OPT, pk_PACKp, USE_BORDER(OPT) },
	{ op_OPTD, cm_OPTD, pk_PACKp, USE_BORDER(OPT) },
	{ op_XOPT, cm_OPT, pk_XPACK, 0, USE_BORDER(XOPT) },
	{ op_TOPT, cm_OPT, pk_TABLE2, USE_BORDER(OPT) },
	{ op_TLOPT, cm_OPT, pk_TABLE, USE_BORDER(OPT) },
	{ op_OKBTN, op_OKBTN, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_CANCELBTN, cm_CANCELBTN, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UCANCELBTN, cm_CANCELBTN, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_OKADD, op_OKADD, pk_XPACK1 | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_BUTTON, cm_BUTTON, pk_XPACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UBUTTON, cm_BUTTON, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) },
// !!! Padding = 5
	{ op_TLBUTTON, cm_BUTTON, pk_TABLEp | pkf_SHOW, 5 },
	{ op_OKTOGGLE, cm_TOGGLE, pk_XPACK1 | pkf_SHOW, 0, USE_BORDER(BUTTON) },
	{ op_UTOGGLE, cm_TOGGLE, pk_PACK | pkf_SHOW, 0, USE_BORDER(BUTTON) }
};

static void do_destroy(void **wdata);

/* V-code is really simple-minded; it can do 0-tests but no arithmetics, and
 * naturally, can inline only constants. Everything else must be prepared either
 * in global variables, or in fields of "ddata" structure.
 * Parameters of codes should be arrayed in fixed order:
 * result location first; frame name last; table location, or name in table,
 * directly before it (last if no frame name); builtin event(s) before that */

#define DEF_BORDER 5
#define GET_BORDER(T) (borders[op_BOR_##T - op_BOR_0] + DEF_BORDER)

void **run_create(void **ifcode, void *ddata, int ddsize)
{
	static const int scrollp[3] = { GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC,
		GTK_POLICY_ALWAYS };
	cmdef *cmds[op_LAST];
#if U_NLS
	char *tc[256];
#endif
#if GTK_MAJOR_VERSION == 1
	int have_sliders = FALSE;
#endif
	int raise = FALSE;
	char txt[PATHTXT];
	int borders[op_BOR_LAST - op_BOR_0], wpos = GTK_WIN_POS_CENTER;
	GtkWindow *tparent = GTK_WINDOW(main_window);
	GtkWidget *wstack[CONT_DEPTH], **wp = wstack + CONT_DEPTH;
	GtkWidget *window = NULL, *widget = NULL;
	GtkAccelGroup* ag = NULL;
	v_dd *vdata;
	void **columns[MAX_COLS], *rstack[CALL_DEPTH], **rp = rstack;
	void *v, **pp, **r = NULL, **res = NULL;
	int ld, dsize;
	int i, n, op, lp, ref, pk, cw, tpad, minw = 0, ncol = 0;


	// Allocation size
	ld = (ddsize + sizeof(void *) - 1) / sizeof(void *);
	n = (sizeof(v_dd) + sizeof(void *) - 1) / sizeof(void *);
	dsize = (ld + n + 2 + predict_size(ifcode, ddata) * 2) * sizeof(void *);
	res = calloc(1, dsize);
	memcpy(res, ddata, ddsize); // Copy datastruct
	ddata = res; // And switch to using it
	vdata = (void *)(res += ld); // Locate where internal data go
	r = res += n; // Anchor after it
	*r++ = ddata; // Store struct ref at anchor
	vdata->code = WDONE; // Make internal datastruct a noop
	*r++ = vdata; // And use it as tag for anchor

	// Border sizes are DEF_BORDER-based
	memset(borders, 0, sizeof(borders));

	// Commands index
	memset(cmds, 0, sizeof(cmds));
	for (i = 0; i < sizeof(cmddefs) / sizeof(cmddefs[0]); i++)
		cmds[cmddefs[i].op] = cmddefs + i;

	while (TRUE)
	{
		op = (int)*ifcode++;
		ifcode = (pp = ifcode) + (lp = op >> 16);
		ref = WB_GETREF(op);
		pk = tpad = cw = 0;
		v = lp ? pp[0] : NULL;
		if (op & WB_FFLAG) v = (void *)((char *)ddata + (int)v);
		op &= WB_OPMASK;
		if (cmds[op])
		{
			tpad = cmds[op]->tpad;
			if (tpad < 0) tpad = borders[-tpad - 1] + DEF_BORDER;
			cw = cmds[op]->cw;
			if (cw < 0) cw = borders[-cw - 1] + DEF_BORDER;
			pk = cmds[op]->pk;
			i = ((pk & pk_MASK) >= pk_TABLE) + !!(pk & pkf_FRAME);
			if (lp <= i) v = NULL;
			op = cmds[op]->cmd;
		}
		switch (op)
		{
		/* Terminate */
		case op_WEND: case op_WSHOW: case op_WDIALOG:
			/* Terminate the list */
			*r++ = NULL;
			*r++ = NULL;

			gtk_signal_connect_object(GTK_OBJECT(window), "destroy",
				GTK_SIGNAL_FUNC(do_destroy), (gpointer)res);

			gtk_window_set_transient_for(GTK_WINDOW(window), tparent);
#if GTK_MAJOR_VERSION == 1
			/* To make Smooth theme engine render sliders properly */
			if (have_sliders) gtk_signal_connect_after(
				GTK_OBJECT(window), "show",
				GTK_SIGNAL_FUNC(gtk_widget_queue_resize), NULL);
#endif
			/* Trigger remembered events */
			trigger_things(res);
			/* Display */
			if (op != op_WEND)
			{
				cmd_showhide(GET_WINDOW(res), TRUE);
				if (raise) gdk_window_raise(window->window);
			}
			/* Wait for input */
			if (op == op_WDIALOG)
			{
				*(void ***)v = NULL; // clear result slot
				vdata->dv = v; // announce it
				while (!*(void ***)v) gtk_main_iteration();
			}
			/* Return anchor position */
			return (res);
		/* Done with a container */
		case op_WDONE: ++wp; continue;
		/* Create a toplevel window, and put a vertical box inside it */
		case op_WINDOWpm:
			v = *(char **)v; // dereference & fallthrough
		case op_WINDOW: case op_WINDOWm:
			widget = window = add_a_window(GTK_WINDOW_TOPLEVEL, _(v),
				wpos, op != op_WINDOW);
			*--wp = add_vbox(window);
			break;
		/* Create a fileselector window, and put a horizontal box inside */
		case op_FPICKpm:
			widget = window = fpick_create(
				*(char **)((char *)ddata + (int)pp[1]),
				*(int *)((char *)ddata + (int)pp[2]));
			gtk_window_set_modal(GTK_WINDOW(window), TRUE);
			*--wp = gtk_hbox_new(FALSE, 0);
			gtk_widget_show(wp[0]);
			fpick_setup(window, wp[0], GTK_SIGNAL_FUNC(get_evt_1_d),
				NEXT_SLOT(r), SLOT_N(r, 2));
			/* Initialize */
			fpick_set_filename(window, v, FALSE);
			break;
		/* Add a notebook page */
		case op_PAGE:
			--wp; wp[0] = widget = add_new_page(wp[1], _(v));
			break;
		/* Add a table */
		case cm_TABLE:
			widget = gtk_table_new((int)v & 0xFFFF, (int)v >> 16, FALSE);
			break;
		/* Add a box */
		case cm_VBOX: case cm_HBOX:
			widget = (op == cm_VBOX ? gtk_vbox_new :
				gtk_hbox_new)(FALSE, (int)v & 255);
			if (pk & pkf_FRAME) break;
			cw = ((int)v >> 8) & 255;
			tpad = ((int)v >> 16) & 255;
			break;
		/* Add an equal-spaced horizontal box */
		case op_EQBOX:
			widget = gtk_hbox_new(TRUE, (int)v & 255);
			cw = (int)v >> 8;
			pk = pk_PACK | pkf_STACK | pkf_SHOW;
			break;
		/* Add a scrolled window */
		case op_SCROLL: case op_XSCROLL:
			widget = gtk_scrolled_window_new(NULL, NULL);
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),
				scrollp[(int)v & 255], scrollp[(int)v >> 8]);
			pk = op == op_SCROLL ? pk_PACK | pkf_STACK | pkf_SHOW :
				pk_XPACK | pkf_STACK | pkf_SHOW;
			break;
		/* Put a notebook into scrolled window */
		case op_SNBOOK:
			widget = gtk_notebook_new();
			gtk_notebook_set_tab_pos(GTK_NOTEBOOK(widget), GTK_POS_LEFT);
//			gtk_notebook_set_scrollable(GTK_NOTEBOOK(widget), TRUE);
			pk = pk_SCROLLVP | pkf_STACK | pkf_SHOW;
			break;
		/* Add a normal notebook */
		case op_NBOOK:
			widget = gtk_notebook_new();
//			gtk_notebook_set_tab_pos(GTK_NOTEBOOK(widget), GTK_POS_TOP);
			cw = GET_BORDER(NBOOK);
			pk = pk_XPACK | pkf_STACK | pkf_SHOW;
			break;
		/* Add a plain notebook (2 pages for now) */
		case op_PLAINBOOK:
		{
			// !!! no extra args
			int n = v ? (int)v : 2; // 2 pages by default
			// !!! All pages go onto stack, with #0 on top
			wp -= n;
			widget = pack(wp[n], plain_book(wp, n));
			break;
		}
		/* Add a toggle button for controlling 2-paged notebook */
		case op_BOOKBTN:
			widget = sig_toggle_button(_(pp[1]), FALSE, v,
				GTK_SIGNAL_FUNC(toggle_vbook));
			pk = pk_PACK;
			break;
		/* Add a horizontal line */
		case op_HSEP:
// !!! Height = 10
			add_hseparator(wp[0], lp ? (int)v : -2, 10);
			continue; // !!! nonreferable: does not return widget
		/* Add a label */
		case op_MLABELp:
			v = *(char **)v; // dereference & fallthrough
		case op_MLABEL: case op_TLLABEL:
			widget = gtk_label_new(_(v));
			if (cw) gtk_misc_set_padding(GTK_MISC(widget), 0, cw);
			cw = 0;
			if (op != op_TLLABEL) break; // Multiline label
			gtk_label_set_justify(GTK_LABEL(widget), GTK_JUSTIFY_LEFT);
			gtk_misc_set_alignment(GTK_MISC(widget), 0.0, 0.5);
			break;
		/* Add a non-spinning spin to table slot */
		case op_TLNOSPIN:
		{
			int n = *(int *)v;
			widget = add_a_spin(n, n, n);
			GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_FOCUS);
			tpad = GET_BORDER(SPIN);
			pk = pk_TABLE;
			break;
		}
		/* Add a spin, fill from field/var */
		case cm_SPIN:
			widget = add_a_spin(*(int *)v, (int)pp[1], (int)pp[2]);
			break;
		/* Add float spin, fill from field/var */
		case cm_FSPIN:
			widget = add_float_spin(*(int *)v / 100.0,
				(int)pp[1] / 100.0, (int)pp[2] / 100.0);
			break;
		/* Add a spin, fill from array */
		case cm_SPINa:
		{
			int *xp = v;
			widget = add_a_spin(xp[0], xp[1], xp[2]);
			break;
		}
		/* Add a spinslider */
		case op_TSPINSLIDE: case op_TLSPINSLIDE: case op_HTSPINSLIDE:
		case cm_SPINSLIDEa:
// !!! Width = 255 Height = 20
			widget = op == op_TSPINSLIDE ? mt_spinslide_new(255, 20) :
				mt_spinslide_new(-1, -1);
			if (op == cm_SPINSLIDEa) mt_spinslide_set_range(widget,
				((int *)v)[1], ((int *)v)[2]);
			else mt_spinslide_set_range(widget, (int)pp[1], (int)pp[2]);
			mt_spinslide_set_value(widget, *(int *)v);
#if GTK_MAJOR_VERSION == 1
			have_sliders = TRUE;
#endif
			if (op == op_HTSPINSLIDE)
			{
				GtkWidget *label;
				int x;

				x = next_table_level(wp[0], TRUE);
				label = gtk_label_new(_(pp[--lp]));
				gtk_widget_show(label);
				gtk_misc_set_alignment(GTK_MISC(label),
					1.0 / 3.0, 0.5);
// !!! Padding = 0
				to_table(label, wp[0], 0, x, 0);
				gtk_table_attach(GTK_TABLE(wp[0]), widget,
					x, x + 1, 1, 2,
					GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
			}
			break;
		/* Add a named checkbox, fill from field/var */
		case cm_CHECK:
			widget = sig_toggle(_(pp[1]), *(int *)v, NULL, NULL);
// !!! Padding = 0
			break;
		/* Add a named checkbox, fill from inifile */
		case op_CHECKb:
			widget = sig_toggle(_(pp[2]), inifile_get_gboolean(v,
				(int)pp[1]), NULL, NULL);
			pk = pk_PACK;
			break;
		/* Add a pack of radio buttons for field/var */
		case cm_RPACK: case cm_RPACKD:
		{
			char **src = pp[1];
			int nh = (int)pp[2];
			int n = nh >> 8;
			if (op == cm_RPACKD) n = -1 ,
				src = *(char ***)((char *)ddata + (int)pp[1]);
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = wj_radio_pack(src, n, nh & 255, *(int *)v,
				ref > 1 ? NEXT_SLOT(r) : NULL,
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_1_t) : NULL);
			break;
		}
		/* Add an option menu for field/var */
		case cm_OPT: case cm_OPTD:
		{
			char **src = pp[1];
			int n = (int)pp[2];
			if (op == cm_OPTD) n = -1 ,
				src = *(char ***)((char *)ddata + (int)pp[1]);
			if (!n) n = -1;
#if U_NLS
			n = n_trans(tc, src, n);
			src = tc;
#endif
			widget = wj_option_menu(src, n, *(int *)v,
				ref > 1 ? NEXT_SLOT(r) : NULL,
				ref > 1 ? GTK_SIGNAL_FUNC(get_evt_1) : NULL);
			break;
		}
		/* Add a multiline entry widget, fill from drop-away buffer */
		case op_MLENTRY:
			widget = gtk_entry_new();
			gtk_entry_set_text(GTK_ENTRY(widget), *(char **)v);
			accept_ctrl_enter(widget);
// !!! Padding = 0 Border = 0
			pk = pk_PACK | pkf_SHOW;
			break;
		/* Add a path entry or box to table */
		case op_TPENTRY: case op_PATH: case op_PATHs:
			if (op == op_TPENTRY)
			{
				widget = gtk_entry_new_with_max_length((int)pp[1]);
// !!! Padding = 0
				pk = pk_TABLE2 | pkf_SHOW;
			}
			else widget = mt_path_box(_(pp[1]), wp[0], _(pp[2]), (int)pp[3]);
			gtkuncpy(txt, op == op_PATHs ? inifile_get(v, "") : v,
				PATHTXT);
			gtk_entry_set_text(GTK_ENTRY(widget), txt);
			break;
		/* Add a text widget, fill from drop-away buffer at pointer */
		case op_TEXT:
			widget = textarea(*(char **)v);
			*(char **)v = NULL;
// !!! Padding = 0 Border = 0
			pk = pk_XPACK | pkf_PARENT; // wrapped
			break;
		/* Add a color picker box, w/field array, & leave unfilled(!) */
		case op_COLOR: case op_TCOLOR:
			widget = cpick_create();
			cpick_set_opacity_visibility(widget, op == op_TCOLOR);
			pk = pk_PACK | pkf_SHOW;
			break;
		/* Add a colorlist box, fill from fields */
		case op_COLORLIST: case op_COLORLISTN:
			widget = colorlist(wp[0], v, ddata, pp - 1, NEXT_SLOT(r));
			break;
		/* Add a colorpad */
		case op_COLORPAD:
			widget = colorpad(v, ddata, pp - 1, NEXT_SLOT(r));
			pk = pk_PACK | pkf_SHOW;
			break;
		/* Add a buttonbar for gradient */
		case op_GRADBAR:
			widget = gradbar(v, ddata, pp - 1, NEXT_SLOT(r));
			pk = pk_PACK;
			break;
		/* Add a list with pre-defined columns */
		case op_LISTCCr:
			widget = listcc(v, ddata, pp - 1, columns, ncol, r);
			ncol = 0;
// !!! Border = 5
			cw = 5;
			pk = pk_SCROLLVP;
			break;
		/* Add a box with "OK"/"Cancel", or something like */
		case op_OKBOX: case op_EOKBOX: case op_UOKBOX:
		{
			GtkWidget *ok_bt, *cancel_bt, *box;

			ag = gtk_accel_group_new();
 			gtk_window_add_accel_group(GTK_WINDOW(window), ag);

			widget = gtk_hbox_new(op != op_UOKBOX, 0);
			pk = pk_PACK | pkf_STACK | pkf_SHOW;
			if (op == op_EOKBOX) // clustered to right side
			{
				box = gtk_hbox_new(FALSE, 0);
				gtk_widget_show(box);
// !!! Min width = 260
				pack_end(box, widget_align_minsize(widget, 260, -1));
				pk = pk_PACK | pkf_STACK | pkf_SHOW | pkf_PARENT;
			}
			cw = GET_BORDER(OKBOX);
			if (ref < 2) break; // empty box for separate buttons

			ok_bt = cancel_bt = gtk_button_new_with_label(_(v));
			gtk_container_set_border_width(GTK_CONTAINER(ok_bt),
				GET_BORDER(BUTTON));
			gtk_widget_show(ok_bt);
			/* OK-event */
			add_click(NEXT_SLOT(r), pp + 2, ok_bt, TRUE);
			if (pp[1])
			{
				cancel_bt = xpack(widget,
					gtk_button_new_with_label(_(pp[1])));
				gtk_container_set_border_width(
					GTK_CONTAINER(cancel_bt), GET_BORDER(BUTTON));
				gtk_widget_show(cancel_bt);
				/* Cancel-event */
				add_click(SLOT_N(r, 2), pp + 4, cancel_bt, TRUE);
			}
			xpack(widget, ok_bt);

			gtk_widget_add_accelerator(cancel_bt, "clicked", ag,
				GDK_Escape, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_Return, 0, (GtkAccelFlags)0);
			gtk_widget_add_accelerator(ok_bt, "clicked", ag,
				GDK_KP_Enter, 0, (GtkAccelFlags)0);
			delete_to_click(window, cancel_bt);
			break;
		}
		/* Add a clickable button */
		case op_OKBTN: case cm_CANCELBTN: case op_OKADD: case cm_BUTTON:
		{
			widget = gtk_button_new_with_label(_(v));
			if (op == op_OKBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Return, 0, (GtkAccelFlags)0);
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_KP_Enter, 0, (GtkAccelFlags)0);
			}
			else if (op == cm_CANCELBTN)
			{
				gtk_widget_add_accelerator(widget, "clicked", ag,
					GDK_Escape, 0, (GtkAccelFlags)0);
				delete_to_click(window, widget);
			}
			/* Click-event */
			add_click(NEXT_SLOT(r), pp + 1, widget, op != cm_BUTTON);
			break;
		}
		/* Add a toggle button to OK-box */
		case cm_TOGGLE:
			widget = gtk_toggle_button_new_with_label(_(pp[1]));
			if (pp[3]) gtk_signal_connect(GTK_OBJECT(widget),
				"toggled", GTK_SIGNAL_FUNC(get_evt_1), NEXT_SLOT(r));
			break;
		/* Call a function */
		case op_EXEC:
			r = ((ext_fn)v)(r, &wp, res);
			continue;
		/* Call a V-code subroutine, indirect from field/var */
// !!! Maybe add opcode for direct call too
		case op_CALLp:
			*rp++ = ifcode;
			ifcode = *(void **)v;
			continue;
		/* Return from V-code subroutine */
		case op_RET:
			ifcode = *--rp;
			continue;
		/* Skip next token(s) if/unless field/var is unset */
		case op_IF: case op_UNLESS:
			if (!*(int *)v ^ (op != op_IF))
				ifcode = skip_if(pp - 1);
			continue;
		/* Store a reference to whatever is next into field */
		case op_REF:
			*(void **)v = r;
			continue;
		/* Make toplevel window shrinkable */
		case op_MKSHRINK:
			gtk_window_set_policy(GTK_WINDOW(window), TRUE, TRUE, FALSE);
			continue;
		/* Make toplevel window non-resizable */
		case op_NORESIZE:
			gtk_window_set_policy(GTK_WINDOW(window), FALSE, FALSE, TRUE);
			continue;
		/* Make scrolled window request max size */
		case op_WANTMAX:
			gtk_signal_connect(GTK_OBJECT(widget), "size_request",
				GTK_SIGNAL_FUNC(scroll_max_size_req), NULL);
			continue;
		/* Use saved size & position for window */
		case op_WXYWH:
		{
			unsigned int n = (unsigned)pp[1];
			vdata->ininame = v;
			vdata->xywh[2] = n >> 16;
			vdata->xywh[3] = n & 0xFFFF;
			if (v) rw_pos(vdata, FALSE);
			continue;
		}
		/* Make toplevel window be positioned at mouse */
		case op_WPMOUSE: wpos = GTK_WIN_POS_MOUSE; continue;
		/* Make toplevel window be positioned anywhere WM pleases */
		case op_WPWHEREVER: wpos = GTK_WIN_POS_NONE; continue;
		/* Make last referrable widget insensitive */
		case op_INSENS:
			gtk_widget_set_sensitive(*origin_slot(r - 2), FALSE);
			continue;
		/* Make last referrable widget focused */
		case op_FOCUS:
			gtk_widget_grab_focus(*origin_slot(r - 2));
			continue;
		/* Set fixed/minimum width for next widget */
		case op_WIDTH:
			minw = (int)v;
			continue;
		/* Make window transient to given widget-map */
		case op_ONTOP:
			tparent = !v ? NULL :
				GTK_WINDOW(GET_REAL_WINDOW(*(void ***)v));
			continue;
		/* Raise window after displaying */
		case op_RAISED:
			raise = TRUE;
			continue;
		/* Start group of list columns */
		case op_WLIST:
			ncol = 0;
			continue;
		/* Add a list column */
		case op_IDXCOL: case op_TXTCOL:
			columns[ncol++] = r;
			*r++ = NULL;
			*r++ = pp - 1;
			continue;
		/* Install Change-event handler */
		case op_EVT_CHANGE:
		{
			void **slot = origin_slot(r - 2);
			int what = GET_OP(slot);
// !!! Support only what actually used on, and their brethren
			switch (what)
			{
			case op_SPIN: case op_XSPIN:
			case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
			case op_SPINa: case op_XSPINa: case op_TSPINa:
			case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
				spin_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_TSPINSLIDE: case op_TLSPINSLIDE:
			case op_HTSPINSLIDE: case op_SPINSLIDEa:
			case op_XSPINSLIDEa:
				mt_spinslide_connect(*slot,
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_CHECK: case op_XCHECK: case op_TLCHECK:
			case op_CHECKb:
				gtk_signal_connect(GTK_OBJECT(*slot), "toggled",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_COLOR: case op_TCOLOR:
				gtk_signal_connect(GTK_OBJECT(*slot), "color_changed",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			case op_TEXT:
#if GTK_MAJOR_VERSION == 2 /* In GTK+1, same handler as for GtkEntry */
				g_signal_connect(GTK_TEXT_VIEW(*slot)->buffer,
					"changed", GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
#endif
			case op_MLENTRY: case op_TPENTRY:
			case op_PATH: case op_PATHs:
				gtk_signal_connect(GTK_OBJECT(*slot), "changed",
					GTK_SIGNAL_FUNC(get_evt_1), r);
				break;
			}
		} // fallthrough
		/* Remember that event needs triggering here */
		/* Or remember start of a group of widgets */
		/* Or a cleanup location */
		case op_TRIGGER: case op_GROUP: case op_CLEANUP:
			*r++ = res;
			*r++ = pp - 1;
			continue;
		/* Set nondefault border size */
		case op_BOR_TABLE: case op_BOR_NBOOK: case op_BOR_SPIN:
		case op_BOR_LABEL: case op_BOR_TLABEL:
		case op_BOR_OPT: case op_BOR_XOPT:
		case op_BOR_FRBOX: case op_BOR_OKBOX: case op_BOR_BUTTON:
			borders[op - op_BOR_0] = lp ? (int)v - DEF_BORDER : 0;
			continue;
		default: continue;
		}
		/* Remember this */
		if (ref)
		{
			*r++ = widget;
			*r++ = pp - 1;
		}
		*(wp - 1) = widget; // pre-stack
		/* Show this */
		if (pk & pkf_SHOW) gtk_widget_show(widget);
		/* Border this */
		if (cw) gtk_container_set_border_width(GTK_CONTAINER(widget), cw);
		/* Unwrap this */
		if (pk & pkf_PARENT)
			while (widget->parent) widget = widget->parent;
		/* Frame this */
		if (pk & pkf_FRAME)
			widget = add_with_frame(NULL, _(pp[--lp]), widget);
		/* Set min width for this */
// !!! For now, always use wrapper
		if (minw < 0) widget = widget_align_minsize(widget, -minw, -2);
		/* Or fixed width */
		else if (minw) gtk_widget_set_usize(widget, minw, -2);
		minw = 0;
		/* Pack this */
		switch (n = pk & pk_MASK)
		{
		case pk_PACK: tpad = 0;
		case pk_PACKp:
			gtk_box_pack_start(GTK_BOX(wp[0]), widget,
				FALSE, FALSE, tpad);
			break;
		case pk_XPACK: case pk_XPACK1:
			gtk_box_pack_start(GTK_BOX(wp[0]), widget,
				TRUE, TRUE, tpad);
			if (n == pk_XPACK1)
				gtk_box_reorder_child(GTK_BOX(wp[0]), widget, 1);
			break;
		case pk_PACKEND: pack_end(wp[0], widget); break;
		case pk_TABLE: case pk_TABLEx: case pk_TABLEp:
			table_it(wp[0], widget, (int)pp[--lp], tpad, n);
			break;
		case pk_TABLE2: case pk_TABLE2x:
		{
			int y = next_table_level(wp[0], FALSE);
			add_to_table(_(pp[--lp]), wp[0], y, 0, GET_BORDER(TLABEL));
			gtk_table_attach(GTK_TABLE(wp[0]), widget, 1, 2,
				y, y + 1, GTK_EXPAND | GTK_FILL,
				n == pk_TABLE2x ? GTK_FILL : 0, 0, tpad);
			break;
		}
		case pk_SCROLLVP: case pk_SCROLLVPn:
		{
			GtkWidget *tw = wp[0];

			wp[0] = *(wp - 1); ++wp; // unstack
			gtk_scrolled_window_add_with_viewport(
				GTK_SCROLLED_WINDOW(tw), widget);
			if (n != pk_SCROLLVPn) break;
			/* Set viewport to shadowless */
			tw = GTK_BIN(tw)->child;
			gtk_viewport_set_shadow_type(GTK_VIEWPORT(tw), GTK_SHADOW_NONE);
			vport_noshadow_fix(tw);
			break;
		}
		}
		/* Stack this */
		if (pk & pkf_STACK) --wp;
		/* Remember events */
		if (ref > 2)
		{
			*r++ = res;
			*r++ = pp + lp - 4;
		}
		if (ref > 1)
		{
			*r++ = res;
			*r++ = pp + lp - 2;
		}
	}
}

static void do_destroy(void **wdata)
{
	void **pp, *v = NULL;
	char *data = GET_DDATA(wdata);
	int op;

	for (wdata = GET_WINDOW(wdata); (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = pp[0];
		if (op & WB_FFLAG) v = data + (int)v;
		op &= WB_OPMASK;
		if (op == op_CLEANUP) free(*(void **)v);
		else if (op == op_TEXT) g_free(*(char **)v);
	}
	free(data);
}

static void *do_query(char *data, void **wdata, int mode)
{
	void **pp, *v = NULL;
	int op;

	for (; (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = op & (~0 << WB_LSHIFT) ? pp[0] : NULL;
		if (op & WB_FFLAG) v = data + (int)v;
		op &= WB_OPMASK;
		switch (op)
		{
		case op_FPICKpm:
			fpick_get_filename(*wdata, v, PATHBUF, FALSE);
			break;
		case op_SPIN: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
		case op_SPINa: case op_XSPINa: case op_TSPINa:
			*(int *)v = mode & 1 ? gtk_spin_button_get_value_as_int(
				GTK_SPIN_BUTTON(*wdata)) : read_spin(*wdata);
			break;
		case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
			*(int *)v = rint((mode & 1 ?
				GTK_SPIN_BUTTON(*wdata)->adjustment->value :
				read_float_spin(*wdata)) * 100);
			break;
		case op_TSPINSLIDE: case op_TLSPINSLIDE: case op_HTSPINSLIDE:
		case op_SPINSLIDEa: case op_XSPINSLIDEa:
			*(int *)v = (mode & 1 ? mt_spinslide_read_value :
				mt_spinslide_get_value)(*wdata);
			break;
		case op_CHECK: case op_XCHECK: case op_TLCHECK:
		case op_OKTOGGLE: case op_UTOGGLE:
			*(int *)v = gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata));
			break;
		case op_CHECKb:
			inifile_set_gboolean(v, gtk_toggle_button_get_active(
				GTK_TOGGLE_BUTTON(*wdata)));
			break;
		case op_COLORLIST: case op_COLORLISTN:
		case op_COLORPAD: case op_GRADBAR: case op_LISTCCr:
			break; // self-reading
		case op_COLOR:
			*(int *)v = cpick_get_colour(*wdata, NULL);
			break;
		case op_TCOLOR:
			*(int *)v = cpick_get_colour(*wdata, (int *)v + 1);
			break;
		case op_RPACK: case op_RPACKD: case op_FRPACK:
			*(int *)v = wj_radio_pack_get_active(*wdata);
			break;
		case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT:
		case op_OPTD:
			*(int *)v = wj_option_menu_get_history(*wdata);
			break;
		case op_MLENTRY:
			*(const char **)v = gtk_entry_get_text(GTK_ENTRY(*wdata));
			break;
		case op_TPENTRY: case op_PATH:
			gtkncpy(v, gtk_entry_get_text(GTK_ENTRY(*wdata)),
				op == op_TPENTRY ? (int)pp[1] : PATHBUF);
			break;
		case op_PATHs:
		{
			char path[PATHBUF];
			gtkncpy(path, gtk_entry_get_text(GTK_ENTRY(*wdata)), PATHBUF);
			inifile_set(v, path);
			break;
		}
		case op_TEXT:
			g_free(*(char **)v);
			*(char **)v = read_textarea(*wdata);
			break;
		default: v = NULL; break;
		}
		if (mode > 1) return (v);
	}
	return (NULL);
}

void run_query(void **wdata)
{
	update_window_spin(GET_REAL_WINDOW(wdata));
	do_query(GET_DDATA(wdata), GET_WINDOW(wdata), 0);
}

void run_destroy(void **wdata)
{
	v_dd *vdata = GET_VDATA(wdata);
	if (vdata->ininame && vdata->ininame[0])
		cmd_showhide(GET_WINDOW(wdata), FALSE); // save position & size
	destroy_dialog(GET_REAL_WINDOW(wdata));
}

void run_reset(void **wdata, int group)
{
// !!! Support only what actually used on, and their brethren
	char *data = GET_DDATA(wdata);
	void **pp, *v = NULL;
	int op, cgroup = 0;

	wdata = GET_WINDOW(wdata);
	for (; (pp = wdata[1]); wdata += 2)
	{
		op = (int)*pp++;
		v = op & (~0 << 16) ? pp[0] : NULL;
		if (op & WB_FFLAG) v = data + (int)v;
		op &= WB_OPMASK;
		if (op == op_GROUP) cgroup = (int)v;
		if (cgroup != group) continue;
		switch (op)
		{
		case op_SPIN: case op_XSPIN:
		case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
		case op_SPINa: case op_XSPINa: case op_TSPINa:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(*wdata),
				*(int *)v);
			break;
		case op_TSPINSLIDE: case op_TLSPINSLIDE: case op_HTSPINSLIDE:
		case op_SPINSLIDEa: case op_XSPINSLIDEa:
			mt_spinslide_set_value(*wdata, *(int *)v);
			break;
		case op_CHECK: case op_XCHECK: case op_TLCHECK:
		case op_OKTOGGLE: case op_UTOGGLE:
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*wdata),
				*(int *)v);
			break;
		case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT:
		case op_OPTD:
			gtk_option_menu_set_history(GTK_OPTION_MENU(*wdata),
				*(int *)v);
			break;
#if 0 /* Not needed for now */
		case op_FPICKpm:
			fpick_set_filename(*wdata, v, FALSE);
			break;
		case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(*wdata),
				*(int *)v * 0.01);
			break;
		case op_CHECKb:
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(*wdata),
				inifile_get_gboolean(v, (int)pp[1]));
			break;
		case op_RPACK: case op_RPACKD: case op_FRPACK:
		case op_COLORLIST: case op_COLORLISTN:
// !!! No ready setter functions for these (and no need of them yet)
			break;
		case op_COLOR:
			cpick_set_colour(slot[0], *(int *)v, 255);
			break;
		case op_TCOLOR:
			cpick_set_colour(slot[0], ((int *)v)[0], ((int *)v)[1]);
			break;
		case op_PATH: case op_PATHs:
		{
			char path[PATHTXT];
			gtkuncpy(path, op == op_PATHs ? inifile_get(v, "") : v,
				PATHTXT);
			gtk_entry_set_text(GTK_ENTRY(*wdata), txt);
			break;
		}
#endif
		}
	}
}

void cmd_sensitive(void **slot, int state)
{
	if (GET_OP(slot) < op_EVT_0) // any widget
		gtk_widget_set_sensitive(slot[0], state);
}

void cmd_showhide(void **slot, int state)
{
	if (GET_OP(slot) >= op_EVT_0) return; // only widgets
	if (!GTK_WIDGET_VISIBLE(slot[0]) ^ !!state) return; // no use
	if (GET_OP(PREV_SLOT(slot)) == op_WDONE) // toplevels are special
	{
		v_dd *vdata = GET_VDATA(PREV_SLOT(slot));
		GtkWidget *w = GTK_WIDGET(slot[0]);

		if (state) // show - apply stored size & position
		{
			if (vdata->ininame) gtk_widget_set_uposition(w,
				vdata->xywh[0], vdata->xywh[1]);
			else vdata->ininame = ""; // first time
			gtk_window_set_default_size(GTK_WINDOW(w),
				vdata->xywh[2] ? vdata->xywh[2] : -1,
				vdata->xywh[3] ? vdata->xywh[3] : -1);
		}
		else // hide - remember size & position
		{
			gdk_window_get_size(w->window,
				vdata->xywh + 2, vdata->xywh + 3);
			gdk_window_get_root_origin(w->window,
				vdata->xywh + 0, vdata->xywh + 1);
			if (vdata->ininame && vdata->ininame[0])
				rw_pos(vdata, TRUE);
		}
	}
	widget_showhide(slot[0], state);
}

void cmd_set(void **slot, int v)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_TSPINSLIDE: case op_TLSPINSLIDE: case op_HTSPINSLIDE:
	case op_SPINSLIDEa: case op_XSPINSLIDEa:
		mt_spinslide_set_value(slot[0], v);
		break;
	case op_TLNOSPIN:
		spin_set_range(slot[0], v, v);
		break;
	case op_SPIN: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		gtk_spin_button_set_value(slot[0], v);
		break;
	case op_FSPIN: case op_TFSPIN: case op_TLFSPIN:
		gtk_spin_button_set_value(slot[0], v / 100.0);
		break;
	case op_CHECK: case op_XCHECK: case op_TLCHECK:
	case op_OKTOGGLE: case op_UTOGGLE:
		gtk_toggle_button_set_active(slot[0], v);
		break;
	case op_OPT: case op_XOPT: case op_TOPT: case op_TLOPT: case op_OPTD:
		gtk_option_menu_set_history(slot[0], v);
		break;
	case op_COLORPAD:
		colorpad_set(slot, v);
		break;
	case op_PLAINBOOK:
		gtk_notebook_set_page(slot[0], v);
		break;
	}
}

void cmd_set2(void **slot, int v0, int v1)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_COLOR: case op_TCOLOR:
		cpick_set_colour(slot[0], v0, v1);
		break;
	case op_COLORLIST: case op_COLORLISTN:
	{
		colorlist_set_color(slot[0], v0, v1);
		break;
	}
	}
}

void cmd_set3(void **slot, int *v)
{
// !!! Support only what actually used on, and their brethren
	switch (GET_OP(slot))
	{
	case op_TSPINSLIDE: case op_TLSPINSLIDE: case op_HTSPINSLIDE:
	case op_SPINSLIDEa: case op_XSPINSLIDEa:
		mt_spinslide_set_range(slot[0], v[1], v[2]);
		mt_spinslide_set_value(slot[0], v[0]);
		break;
	case op_SPIN: case op_XSPIN:
	case op_TSPIN: case op_TLSPIN: case op_TLXSPIN:
	case op_SPINa: case op_XSPINa: case op_TSPINa:
		spin_set_range(slot[0], v[1], v[2]);
		gtk_spin_button_set_value(slot[0], v[0]);
		break;
	}
}

void cmd_set4(void **slot, int *v)
{
// !!! Support only what actually used on, and their brethren
	int op = GET_OP(slot);
	if ((op == op_COLOR) || (op == op_TCOLOR))
	{
		cpick_set_colour_previous(slot[0], v[2], v[3]);
		cpick_set_colour(slot[0], v[0], v[1]);
	}
}

void cmd_setlist(void **slot, char *map, int n)
{
// !!! Support only what actually used on, and their brethren
	int op = GET_OP(slot);
	if ((op == op_OPT) || (op == op_XOPT) || (op == op_TOPT) ||
		(op == op_TLOPT) || (op == op_OPTD))
	{
		GList *items = GTK_MENU_SHELL(gtk_option_menu_get_menu(
			GTK_OPTION_MENU(slot[0])))->children;
		int i, j, k;

		for (i = j = 0; items; items = items->next , i++)
		{
			k = i < n ? map[i] : 0; // show/hide
			if (k > 1) j = i; // select
			widget_showhide(GTK_WIDGET(items->data), k);
		}
		gtk_option_menu_set_history(GTK_OPTION_MENU(slot[0]), j);
	}
}

/* Passively query one slot, show where the result went */
void *cmd_read(void **slot, void *ddata)
{
	return (do_query(ddata, origin_slot(slot), 3));
}

void cmd_peekv(void **slot, void *res, int size, int idx)
{
// !!! Support only what actually used on
	int op = GET_OP(slot);
	if (op == op_FPICKpm) fpick_get_filename(slot[0], res, size, idx);
}

void cmd_setv(void **slot, void *res, int idx)
{
// !!! Support only what actually used on
	int op = GET_OP(slot);
	if (op == op_FPICKpm) fpick_set_filename(slot[0], res, idx);
	else if (op == op_TEXT) set_textarea(slot[0], res);
}

void cmd_repaint(void **slot)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
	/* Stupid GTK+ does nothing for gtk_widget_queue_draw(allcol_list) */
		gtk_container_foreach(GTK_CONTAINER(slot[0]),
			(GtkCallback)gtk_widget_queue_draw, NULL);
	else gtk_widget_queue_draw(slot[0]);
}

void cmd_scroll(void **slot, int idx)
{
	int op = GET_OP(slot);
	if ((op == op_COLORLIST) || (op == op_COLORLISTN))
		colorlist_scroll_in(slot[0], idx);
}

// !!! GTK-specific for now
void cmd_cursor(void **slot, GdkCursor *cursor)
{
	gdk_window_set_cursor(GTK_WIDGET(slot[0])->window, cursor);
}
