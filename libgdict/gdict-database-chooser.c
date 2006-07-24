/* gdict-database-chooser.c - display widget for database names
 *
 * Copyright (C) 2006  Emmanuele Bassi <ebassi@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gtk/gtkbindings.h>
#include <glib/gi18n.h>

#include "gdict-database-chooser.h"
#include "gdict-utils.h"
#include "gdict-private.h"
#include "gdict-enum-types.h"
#include "gdict-marshal.h"

#define GDICT_DATABASE_CHOOSER_GET_PRIVATE(obj) \
(G_TYPE_INSTANCE_GET_PRIVATE ((obj), GDICT_TYPE_DATABASE_CHOOSER, GdictDatabaseChooserPrivate))

struct _GdictDatabaseChooserPrivate
{
  GtkListStore *store;

  GtkWidget *treeview;
  GtkWidget *clear_button;
  GtkWidget *refresh_button;
  
  GdictContext *context;
  gint results;

  GtkTooltips *tips;
  
  guint start_id;
  guint match_id;
  guint end_id;
  guint error_id;

  GdkCursor *busy_cursor;

  guint is_searching : 1;
};

enum
{
  DATABASE_NAME,
  DATABASE_ERROR
} DBType;

enum
{
  DB_COLUMN_TYPE,
  DB_COLUMN_NAME,
  DB_COLUMN_DESCRIPTION,

  DB_N_COLUMNS
};

enum
{
  PROP_0,
  
  PROP_CONTEXT,
  PROP_COUNT
};

enum
{
  DATABASE_ACTIVATED,
  CLOSED,

  LAST_SIGNAL
};

static guint db_chooser_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GdictDatabaseChooser,
               gdict_database_chooser,
               GTK_TYPE_VBOX);


static void
set_gdict_context (GdictDatabaseChooser *chooser,
		   GdictContext         *context)
{
  GdictDatabaseChooserPrivate *priv;
  
  g_assert (GDICT_IS_DATABASE_CHOOSER (chooser));
  priv = chooser->priv;
  
  if (priv->context)
    {
      if (priv->start_id)
        {
          _gdict_debug ("Removing old context handlers\n");
          
          g_signal_handler_disconnect (priv->context, priv->start_id);
          g_signal_handler_disconnect (priv->context, priv->match_id);
          g_signal_handler_disconnect (priv->context, priv->end_id);
          
          priv->start_id = 0;
          priv->end_id = 0;
          priv->match_id = 0;
        }
      
      if (priv->error_id)
        {
          g_signal_handler_disconnect (priv->context, priv->error_id);

          priv->error_id = 0;
        }

      _gdict_debug ("Removing old context\n");
      
      g_object_unref (G_OBJECT (priv->context));
    }

  if (!context)
    return;

  if (!GDICT_IS_CONTEXT (context))
    {
      g_warning ("Object of type '%s' instead of a GdictContext\n",
      		 g_type_name (G_OBJECT_TYPE (context)));
      return;
    }

  _gdict_debug ("Setting new context\n");
    
  priv->context = context;
  g_object_ref (G_OBJECT (priv->context));
}

static void
gdict_database_chooser_finalize (GObject *gobject)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (gobject);
  GdictDatabaseChooserPrivate *priv = chooser->priv;

  if (priv->context)
    set_gdict_context (chooser, NULL);

  if (priv->busy_cursor)
    gdk_cursor_unref (priv->busy_cursor);

  g_object_unref (priv->store);

  if (priv->tips)
    g_object_unref (priv->tips);
  
  G_OBJECT_CLASS (gdict_database_chooser_parent_class)->finalize (gobject);
}

static void
gdict_database_chooser_set_property (GObject      *gobject,
				     guint         prop_id,
				     const GValue *value,
				     GParamSpec   *pspec)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (gobject);
  
  switch (prop_id)
    {
    case PROP_CONTEXT:
      set_gdict_context (chooser, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
gdict_database_chooser_get_property (GObject    *gobject,
				     guint       prop_id,
				     GValue     *value,
				     GParamSpec *pspec)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (gobject);
  
  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, chooser->priv->context);
      break;
    case PROP_COUNT:
      g_value_set_int (value, chooser->priv->results);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
row_activated_cb (GtkTreeView       *treeview,
		  GtkTreePath       *path,
		  GtkTreeViewColumn *column,
		  gpointer           user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);
  GdictDatabaseChooserPrivate *priv = chooser->priv;
  GtkTreeIter iter;
  gchar *db_name, *db_desc;
  gboolean valid;

  valid = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store),
		  		   &iter,
				   path);
  if (!valid)
    {
      g_warning ("Invalid iterator found");
      return;
    }

  gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
		      DB_COLUMN_NAME, &db_name,
		      DB_COLUMN_DESCRIPTION, &db_desc,
		      -1);
  if (db_name && db_desc)
    {
      g_signal_emit (chooser, db_chooser_signals[DATABASE_ACTIVATED], 0,
		     db_name, db_desc);
    }
  else
    {
      gchar *row = gtk_tree_path_to_string (path);

      g_warning ("Row %s activated, but no database attached", row);
      g_free (row);
    }

  g_free (db_name);
  g_free (db_desc);
}

static void
refresh_button_clicked_cb (GtkWidget *widget,
			   gpointer   user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);

  gdict_database_chooser_refresh (chooser);
}

static void
clear_button_clicked_cb (GtkWidget *widget,
			 gpointer   user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);

  gdict_database_chooser_clear (chooser);
}

static GObject *
gdict_database_chooser_constructor (GType                  type,
				    guint                  n_params,
				    GObjectConstructParam *params)
{
  GObject *object;
  GdictDatabaseChooser *chooser;
  GdictDatabaseChooserPrivate *priv;
  GtkWidget *sw;
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;
  GtkWidget *hbox;

  object = G_OBJECT_CLASS (gdict_database_chooser_parent_class)->constructor (type,
		  							      n_params,
									      params);

  chooser = GDICT_DATABASE_CHOOSER (object);
  priv = chooser->priv;

  gtk_widget_push_composite_child ();

  sw = gtk_scrolled_window_new (NULL, NULL);
  gtk_widget_set_composite_name (sw, "gdict-database-chooser-scrolled-window");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
		  		  GTK_POLICY_AUTOMATIC,
				  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw),
		  		       GTK_SHADOW_IN);
  gtk_box_pack_start (GTK_BOX (chooser), sw, TRUE, TRUE, 0);
  gtk_widget_show (sw);

  renderer = gtk_cell_renderer_text_new ();
  column = gtk_tree_view_column_new_with_attributes ("databases",
		  				     renderer,
						     "text", DB_COLUMN_DESCRIPTION,
						     NULL);
  priv->treeview = gtk_tree_view_new ();
  gtk_widget_set_composite_name (priv->treeview, "gdict-database-chooser-treeview");
  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->treeview),
		  	   GTK_TREE_MODEL (priv->store));
  gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (priv->treeview), FALSE);
  gtk_tree_view_append_column (GTK_TREE_VIEW (priv->treeview), column);
  g_signal_connect (priv->treeview, "row-activated",
		    G_CALLBACK (row_activated_cb), chooser);
  gtk_container_add (GTK_CONTAINER (sw), priv->treeview);
  gtk_widget_show (priv->treeview);

  hbox = gtk_hbox_new (FALSE, 0);

  priv->refresh_button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (priv->refresh_button),
		        gtk_image_new_from_stock (GTK_STOCK_REFRESH,
						  GTK_ICON_SIZE_SMALL_TOOLBAR));
  g_signal_connect (priv->refresh_button, "clicked",
		    G_CALLBACK (refresh_button_clicked_cb),
		    chooser);
  gtk_box_pack_start (GTK_BOX (hbox), priv->refresh_button, FALSE, FALSE, 0);
  gtk_widget_show (priv->refresh_button);
  gtk_tooltips_set_tip (priv->tips, priv->refresh_button,
		  	_("Reload the list of available databases"),
			NULL);

  priv->clear_button = gtk_button_new ();
  gtk_button_set_image (GTK_BUTTON (priv->clear_button),
		        gtk_image_new_from_stock (GTK_STOCK_CLEAR,
						  GTK_ICON_SIZE_SMALL_TOOLBAR));
  g_signal_connect (priv->clear_button, "clicked",
		    G_CALLBACK (clear_button_clicked_cb),
		    chooser);
  gtk_box_pack_start (GTK_BOX (hbox), priv->clear_button, FALSE, FALSE, 0);
  gtk_widget_show (priv->clear_button);
  gtk_tooltips_set_tip (priv->tips, priv->clear_button,
		        _("Clear the list of available databases"),
			NULL);

  gtk_box_pack_end (GTK_BOX (chooser), hbox, FALSE, FALSE, 0);
  gtk_widget_show (hbox);
  
  gtk_widget_pop_composite_child ();

  return object;
}

static void
gdict_database_chooser_class_init (GdictDatabaseChooserClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = gdict_database_chooser_finalize;
  gobject_class->set_property = gdict_database_chooser_set_property;
  gobject_class->get_property = gdict_database_chooser_get_property;
  gobject_class->constructor = gdict_database_chooser_constructor;
  
  g_object_class_install_property (gobject_class,
  				   PROP_CONTEXT,
  				   g_param_spec_object ("context",
  				   			"Context",
  				   			"The GdictContext object used to get the list of databases",
  				   			GDICT_TYPE_CONTEXT,
  				   			(G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT)));

  db_chooser_signals[DATABASE_ACTIVATED] =
    g_signal_new ("database-activated",
		  G_OBJECT_CLASS_TYPE (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GdictDatabaseChooserClass, database_activated),
		  NULL, NULL,
		  gdict_marshal_VOID__STRING_STRING,
		  G_TYPE_NONE, 2,
		  G_TYPE_STRING,
		  G_TYPE_STRING);
  
  g_type_class_add_private (gobject_class, sizeof (GdictDatabaseChooserPrivate));
}

static void
gdict_database_chooser_init (GdictDatabaseChooser *chooser)
{
  GdictDatabaseChooserPrivate *priv;

  chooser->priv = priv = GDICT_DATABASE_CHOOSER_GET_PRIVATE (chooser);

  priv->results = -1;
  priv->context = NULL;

  priv->store = gtk_list_store_new (DB_N_COLUMNS,
		                    G_TYPE_INT,    /* DBType */
		                    G_TYPE_STRING, /* db_name */
				    G_TYPE_STRING  /* db_desc */);

  priv->tips = gtk_tooltips_new ();
  g_object_ref_sink (G_OBJECT (priv->tips));

  priv->start_id = 0;
  priv->end_id = 0;
  priv->match_id = 0;
  priv->error_id = 0;
}

/**
 * gdict_database_chooser_new:
 *
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.9
 */
GtkWidget *
gdict_database_chooser_new (void)
{
  return g_object_new (GDICT_TYPE_DATABASE_CHOOSER, NULL);
}

/**
 * gdict_database_chooser_new_with_context:
 * @context: a #GdictContext
 *
 * FIXME
 *
 * Return value: FIXME
 *
 * Since: 0.9
 */
GtkWidget *
gdict_database_chooser_new_with_context (GdictContext *context)
{
  g_return_val_if_fail (GDICT_IS_CONTEXT (context), NULL);
  
  return g_object_new (GDICT_TYPE_DATABASE_CHOOSER,
                       "context", context,
                       NULL);
}

GdictContext *
gdict_database_chooser_get_context (GdictDatabaseChooser *chooser)
{
  g_return_val_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser), NULL);
  
  return chooser->priv->context;
}

void
gdict_database_chooser_set_context (GdictDatabaseChooser *chooser,
				    GdictContext         *context)
{
  g_return_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser));
  g_return_if_fail (context == NULL || GDICT_IS_CONTEXT (context));

  set_gdict_context (chooser, context);

  g_object_notify (G_OBJECT (chooser), "context");
}

gchar **
gdict_database_chooser_get_databases (GdictDatabaseChooser  *chooser,
				      gsize                  length)
{
  g_return_val_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser), NULL);

  return NULL;
}

gboolean
gdict_database_chooser_has_database (GdictDatabaseChooser *chooser,
				     const gchar          *database)
{
  g_return_val_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser), FALSE);
  g_return_val_if_fail (database != NULL, FALSE);

  return FALSE;
}

gint
gdict_database_chooser_count_databases (GdictDatabaseChooser *chooser)
{
  g_return_val_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser), -1);

  return chooser->priv->results;
}

static void
lookup_start_cb (GdictContext *context,
		 gpointer      user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);
  GdictDatabaseChooserPrivate *priv = chooser->priv;

  if (!priv->busy_cursor)
    priv->busy_cursor = gdk_cursor_new (GDK_WATCH);

  if (GTK_WIDGET (chooser)->window)
    gdk_window_set_cursor (GTK_WIDGET (chooser)->window, priv->busy_cursor);

  priv->is_searching = TRUE;
}

static void
lookup_end_cb (GdictContext *context,
	       gpointer      user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);
  GdictDatabaseChooserPrivate *priv = chooser->priv;

  if (GTK_WIDGET (chooser)->window)
    gdk_window_set_cursor (GTK_WIDGET (chooser)->window, NULL);

  priv->is_searching = FALSE;
}

static void
database_found_cb (GdictContext  *context,
		   GdictDatabase *database,
		   gpointer       user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);
  GdictDatabaseChooserPrivate *priv = chooser->priv;
  GtkTreeIter iter;

  _gdict_debug ("DATABASE: `%s' (`%s')\n",
		gdict_database_get_name (database),
		gdict_database_get_full_name (database));

  gtk_list_store_append (priv->store, &iter);
  gtk_list_store_set (priv->store, &iter,
		      DB_COLUMN_TYPE, DATABASE_NAME,
		      DB_COLUMN_NAME, gdict_database_get_name (database),
		      DB_COLUMN_DESCRIPTION, gdict_database_get_full_name (database),
		      -1);

  if (priv->results == -1)
    priv->results = 1;
  else
    priv->results += 1;
}

static void
error_cb (GdictContext *context,
          const GError *error,
	  gpointer      user_data)
{
  GdictDatabaseChooser *chooser = GDICT_DATABASE_CHOOSER (user_data);

  if (GTK_WIDGET (chooser)->window)
    gdk_window_set_cursor (GTK_WIDGET (chooser)->window, NULL);

  chooser->priv->is_searching = FALSE;
}

void
gdict_database_chooser_refresh (GdictDatabaseChooser *chooser)
{
  GdictDatabaseChooserPrivate *priv;
  GError *db_error;
  
  g_return_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser));

  priv = chooser->priv;

  if (!priv->context)
    {
      g_warning ("Attempting to retrieve the available databases, but "
		 "no GdictContext has been set.  Use gdict_database_chooser_set_context() "
		 "before invoking gdict_database_chooser_refresh().");
      return;
    }

  if (priv->is_searching)
    {
      _gdict_show_error_dialog (NULL,
				_("Another search is in progress"),
				_("Please wait until the current search ends."));
      return;
    }

  gdict_database_chooser_clear (chooser);

  if (!priv->start_id)
    {
      priv->start_id = g_signal_connect (priv->context, "lookup-start",
		      			 G_CALLBACK (lookup_start_cb),
					 chooser);
      priv->match_id = g_signal_connect (priv->context, "database-found",
		      			 G_CALLBACK (database_found_cb),
					 chooser);
      priv->end_id = g_signal_connect (priv->context, "lookup-end",
		      		       G_CALLBACK (lookup_end_cb),
				       chooser);
    }

  if (!priv->error_id)
    priv->error_id = g_signal_connect (priv->context, "error",
		    		       G_CALLBACK (error_cb),
				       chooser);

  db_error = NULL;
  gdict_context_lookup_databases (priv->context, &db_error);
  if (db_error)
    {
      GtkTreeIter iter;

      gtk_list_store_append (priv->store, &iter);
      gtk_list_store_set (priv->store, &iter,
		      	  DB_COLUMN_TYPE, DATABASE_ERROR,
			  DB_COLUMN_NAME, _("Error while matching"),
			  DB_COLUMN_DESCRIPTION, NULL,
			  -1);

      _gdict_debug ("Error while searching: %s", db_error->message);
      g_error_free (db_error);
    }
}

void
gdict_database_chooser_clear (GdictDatabaseChooser *chooser)
{
  GdictDatabaseChooserPrivate *priv;

  g_return_if_fail (GDICT_IS_DATABASE_CHOOSER (chooser));

  priv = chooser->priv;

  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->treeview), NULL);

  gtk_list_store_clear (priv->store);
  priv->results = -1;

  gtk_tree_view_set_model (GTK_TREE_VIEW (priv->treeview),
		  	   GTK_TREE_MODEL (priv->store));
}