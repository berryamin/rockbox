/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Robert E. Hak
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
/*
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "button.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings.h"
#include "settings_list.h"
#include "option_select.h"
#include "screens.h"
#include "talk.h"
#include "lang.h"
#include "misc.h"
#include "action.h"
#include "menus/exported_menus.h"
#include "string.h"
#include "root_menu.h"
#include "audio.h"
#include "viewport.h"
#include "quickscreen.h"
#include "shortcuts.h"

#ifdef HAVE_LCD_BITMAP
#include "icons.h"
#endif

/* gui api */
#include "list.h"
#include "buttonbar.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;
static int talk_menu_item(int selected_item, void *data);

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback) 
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}
static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
            return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        return menu->strings[selected_item];
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v
                = find_setting(menu->variable, NULL);
        if (v)
            return str(v->lang_id);
        else return "Not Done yet!";
    }
    return P2STR(menu->callback_and_desc->desc);
}
#ifdef HAVE_LCD_BITMAP
static enum themable_icons  menu_get_icon(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int menu_icon = Icon_NOICON;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
    {
        return Icon_Menu_functioncall;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags&MENU_HAS_DESC)
        menu_icon = menu->callback_and_desc->icon_id;
    else if (menu->flags&MENU_DYNAMIC_DESC)
        menu_icon = menu->menu_get_name_and_icon->icon_id;

    if (menu_icon == Icon_NOICON)
    {
        switch (menu->flags&MENU_TYPE_MASK)
        {
            case MT_SETTING:
            case MT_SETTING_W_TEXT:
                menu_icon = Icon_Menu_setting;
                break;
            case MT_MENU:
                    menu_icon = Icon_Submenu;
                break;
            case MT_FUNCTION_CALL:
            case MT_RETURN_VALUE:
                menu_icon = Icon_Menu_functioncall;
                break;
        }
    }
    return menu_icon;
}
#endif

static void init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS])
{
    int i, count = MENU_GET_COUNT(menu->flags);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = NULL;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
            get_menu_callback(menu->submenus[i],&menu_callback);
        if (menu_callback)
        {
            if (menu_callback(ACTION_REQUEST_MENUITEM,
                    type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i])
                    != ACTION_EXIT_MENUITEM)
            {
                current_subitems[current_subitems_count] = i;
                current_subitems_count++;
            }
        }
        else
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }
    current_submenus_menu = (struct menu_item_ex *)menu;

    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
#ifdef HAVE_LCD_BITMAP

    if (menu->flags&MENU_HAS_DESC)
    {
        icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        char buffer[80];
        icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
    }
    else
    {
        icon = Icon_NOICON;
        title = "";
    }

    if (icon == Icon_NOICON)
            icon = Icon_Submenu_Entered;
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_icon_callback(lists, global_settings.show_icons?menu_get_icon:NULL);
#else
    (void)icon;
    (void)title;
    gui_synclist_set_icon_callback(lists, NULL);
#endif
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(lists, talk_menu_item);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    gui_synclist_limit_scroll(lists,true);
    gui_synclist_select_item(lists, find_menu_selection(selected));

    get_menu_callback(menu,&menu_callback);
    if (callback && menu_callback)
        menu_callback(ACTION_ENTER_MENUITEM,menu);
}

static int talk_menu_item(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int id = -1;
    int type;
    unsigned char *str;
    int sel = get_menu_selection(selected_item, menu);

        if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        {
            type = menu->submenus[sel]->flags&MENU_TYPE_MASK;
            if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
                talk_setting(menu->submenus[sel]->variable);
            else
            {
                if (menu->submenus[sel]->flags&(MENU_DYNAMIC_DESC))
                {
                    int (*list_speak_item)(int selected_item, void * data)
                        = menu->submenus[sel]->menu_get_name_and_icon->
                        list_speak_item;
                    if(list_speak_item)
                        list_speak_item(sel, menu->submenus[sel]->
                                        menu_get_name_and_icon->
                                        list_get_name_data);
                    else
                    {
                        char buffer[80];
                        str = menu->submenus[sel]->menu_get_name_and_icon->
                            list_get_name(sel, menu->submenus[sel]->
                                    menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
                        id = P2ID(str);
                    }
                }
                else
                    id = P2ID(menu->submenus[sel]->callback_and_desc->desc);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        else if(((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID))
        {
            if ((menu->flags&MENU_DYNAMIC_DESC) == 0)
            {
                unsigned char *s = (unsigned char *)menu->strings[sel];
                /* string list, try to talk it if ID2P was used */
                id = P2ID(s);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        return 0;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        if (setting->lang_id == -1)
            title = (char*)setting->cfg_vals;
        else
            title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}
    

void do_setting_from_menu(const struct menu_item_ex *temp,
                          struct viewport parent[NB_SCREENS])
{
    char *title;
    int setting_id;
    const struct settings_list *setting =
        find_setting(temp->variable, &setting_id);
    if (temp && ((temp->flags&MENU_TYPE_MASK) == MT_SETTING_W_TEXT))
        title = temp->callback_and_desc->desc;
    else
        title = ID2P(setting->lang_id);
    do_setting_screen(setting, title, parent);
}

/* display a menu */
int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    int selected = start_selected? *start_selected : 0;
    int action;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;
    int ret = 0;
    bool redraw_lists;
    int old_audio_status = audio_status();

#ifdef HAVE_TOUCHSCREEN
    /* plugins possibly have grid mode active. force global settings in lists */
    enum touchscreen_mode tsm = touchscreen_get_mode();
    touchscreen_set_mode(global_settings.touch_mode);
#endif

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, !hide_theme, NULL);

    const struct menu_item_ex *menu_stack[MAX_MENUS];
    int menu_stack_selected_item[MAX_MENUS];
    int stack_top = 0;
    bool in_stringlist, done = false;
    struct viewport *vps = NULL;
#ifdef HAVE_BUTTONBAR
    struct gui_buttonbar buttonbar;
    gui_buttonbar_init(&buttonbar);
    gui_buttonbar_set_display(&buttonbar, &(screens[SCREEN_MAIN]) );
    gui_buttonbar_set(&buttonbar, "<<<", "", "");
#endif
    menu_callback_type menu_callback = NULL;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen) */
    init_menu_lists(menu, &lists, selected, true, parent);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    gui_synclist_draw(&lists);
    gui_synclist_speak_item(&lists);
#ifdef HAVE_BUTTONBAR
    if (!hide_theme)
    {
        gui_buttonbar_set(&buttonbar, "<<<", "", "");
        gui_buttonbar_draw(&buttonbar);
    }
#endif
    while (!done)
    {
        int new_audio_status;
        redraw_lists = false;
        if (!hide_theme)
        {
#ifdef HAVE_BUTTONBAR
            gui_buttonbar_draw(&buttonbar);
#endif
        }
#if CONFIG_CODEC == SWCODEC
        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);
#endif
        action = get_action(CONTEXT_MAINMENU,
                            list_do_action_timeout(&lists, HZ));

        /* query audio status to see if it changed */
        new_audio_status = audio_status();
        if (old_audio_status != new_audio_status)
        {   /* force a redraw if anything changed the audio status
             * from outside */
            redraw_lists = true;
            old_audio_status = new_audio_status;
        }
        /* HZ so the status bar redraws corectly */

        if (menu_callback)
        {
            int old_action = action;
            action = menu_callback(action, menu);
            if (action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            {
                action = old_action;
                ret = MENU_SELECTED_EXIT; /* will exit after returning
                                             from selection */
            }
            else if (action == ACTION_REDRAW)
            {
                action = old_action;
                redraw_lists = true;
            }
        }

        if (gui_synclist_do_button(&lists, &action, LIST_WRAP_UNLESS_HELD))
            continue;
#ifdef HAVE_QUICKSCREEN
        else if (action == ACTION_STD_QUICKSCREEN)
        {
            quick_screen_quick(action);
            redraw_lists = true;
        }
#endif
#ifdef HAVE_RECORDING
        else if (action == ACTION_STD_REC)
        {
            ret = GO_TO_RECSCREEN;
            done = true;
        }
#endif
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            redraw_lists = list_stop_handler();
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            if (menu == &root_menu_)
            {
                ret = GO_TO_ROOTITEM_CONTEXT;
                done = true;
            }
            else if (!in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                if (type == MT_SETTING_W_TEXT || type == MT_SETTING)
                {
#ifdef HAVE_QUICKSCREEN
                    MENUITEM_STRINGLIST(quickscreen_able_option,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_RESET_SETTING),
                                        ID2P(LANG_TOP_QS_ITEM),
                                        ID2P(LANG_LEFT_QS_ITEM),
                                        ID2P(LANG_BOTTOM_QS_ITEM),
                                        ID2P(LANG_RIGHT_QS_ITEM),
                                        ID2P(LANG_ADD_TO_FAVES));
#endif
                    MENUITEM_STRINGLIST(notquickscreen_able_option, 
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_RESET_SETTING));
                    const struct menu_item_ex *menu;
                    int menu_selection = 0;
                    const struct settings_list *setting = 
                            find_setting(temp->variable, NULL);
#ifdef HAVE_QUICKSCREEN
                    if (is_setting_quickscreenable(setting))
                        menu = &quickscreen_able_option;
                    else
#endif
                        menu = &notquickscreen_able_option;
                    switch (do_menu(menu, &menu_selection, NULL, false))
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* reset setting */
                            reset_setting(setting, setting->setting);
                            settings_save();
                            settings_apply(false);
                            break;
#ifdef HAVE_QUICKSCREEN
                        case 1: /* set as top QS item */
                            set_as_qs_item(setting, QUICKSCREEN_TOP);
                            break;
                        case 2: /* set as left QS item */
                            set_as_qs_item(setting, QUICKSCREEN_LEFT);
                            break;
                        case 3: /* set as bottom QS item */
                            set_as_qs_item(setting, QUICKSCREEN_BOTTOM);
                            break;
                        case 4: /* set as right QS item */
                            set_as_qs_item(setting, QUICKSCREEN_RIGHT);
                            break;
                        case 5: /* Add to faves. Same limitation on which can be
                                  added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING, (void*)setting);
                            break;
#endif
                    } /* swicth(do_menu()) */
                    redraw_lists = true;
                }
            } /* else if (!in_stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            if (menu != &root_menu_)
                ret = GO_TO_ROOT;
            else
                ret = GO_TO_PREVIOUS;
            done = true;
        }
        else if (action == ACTION_STD_CANCEL)
        {
            bool exiting_menu = false;
            in_stringlist = false;
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            if (menu_callback)
                menu_callback(ACTION_EXIT_MENUITEM, menu);

            if (menu->flags&MENU_EXITAFTERTHISMENU)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;
            if (stack_top > 0)
            {
                stack_top--;
                menu = menu_stack[stack_top];
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU))
                    done = true;
                else
                    init_menu_lists(menu, &lists, 
                                    menu_stack_selected_item[stack_top], false, vps);
                redraw_lists = true;
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_)
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            int type = (menu->flags&MENU_TYPE_MASK);
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
#ifdef HAVE_BUTTONBAR
            if (!hide_theme)
            {
                gui_buttonbar_unset(&buttonbar);
                gui_buttonbar_draw(&buttonbar);
            }
#endif
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
                temp = menu->submenus[selected];
            else
                type = -1;

            redraw_lists = true;
            if (in_stringlist)
                type = (menu->flags&MENU_TYPE_MASK);
            else if (temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                get_menu_callback(temp, &menu_callback);
                if (menu_callback)
                {
                    action = menu_callback(ACTION_ENTER_MENUITEM,temp);
                    if (action == ACTION_EXIT_MENUITEM)
                        break;
                }
            }
            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        menu_stack[stack_top] = menu;
                        menu_stack_selected_item[stack_top] = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, true, vps);
                    }
                    break;
                case MT_FUNCTION_CALL:
                {
                    int return_value;
                    if (temp->flags&MENU_FUNC_USEPARAM)
                        return_value = temp->function->function_w_param(
                                    temp->function->param);
                    else
                        return_value = temp->function->function();
                    if (!(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        init_menu_lists(menu, &lists, selected, true, vps);
                    }
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    do_setting_from_menu(temp, vps);
                    send_event(GUI_EVENT_ACTIONUPDATE, (void*)1); /* force a redraw */
                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        menu_stack[stack_top] = menu;
                        menu_stack_selected_item[stack_top] = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu,&lists,0,false, vps);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    ret = temp->value;
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (type != MT_MENU)
            {
                if (menu_callback)
                    menu_callback(ACTION_EXIT_MENUITEM,temp);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu,&lists,selected,true,vps);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) && 
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
#ifdef HAVE_BUTTONBAR
            if (!hide_theme)
            {
                gui_buttonbar_set(&buttonbar, "<<<", "", "");
                gui_buttonbar_draw(&buttonbar);
            }
#endif
        }
        else
        {
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        if (redraw_lists && !done)
        {
            if (menu_callback)
                menu_callback(ACTION_REDRAW, menu);
            gui_synclist_draw(&lists);
            gui_synclist_speak_item(&lists);
        }
    }

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = menu_stack[0];
            init_menu_lists(menu,&lists,menu_stack_selected_item[0],true, vps);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
#ifdef HAVE_TOUCHSCREEN
    touchscreen_set_mode(tsm);
#endif

    return ret;
}
