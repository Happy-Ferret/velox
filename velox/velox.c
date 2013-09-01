/* velox: velox/velox.c
 *
 * Copyright (c) 2009, 2010 Michael Forney <mforney@mforney.org>
 *
 * This file is a part of velox.
 *
 * velox is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2, as published by the Free
 * Software Foundation.
 *
 * velox is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along
 * with velox.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#include "velox.h"
#include "window.h"
#include "workspace.h"
#include "hook.h"
#include "config_file.h"
#include "debug.h"
#include "list.h"
#include "modifier.h"
#include "resource.h"

#include "module-private.h"
#include "config_file-private.h"
#include "hook-private.h"
#include "layout-private.h"
#include "work_area-private.h"
#include "binding-private.h"

#define ARRAY_LENGTH(array) (sizeof (array) / sizeof (array)[0])

/* VELOX variables */
volatile sig_atomic_t running = true;
volatile sig_atomic_t clock_tick_update = true;
struct velox_workspace * active_workspace = NULL;
struct velox_area screen_area;
struct velox_area work_area;

uint16_t border_width = 2;

/* VELOX constants */
const char wm_name[] = "velox";

static void setup()
{
    setup_hooks();
    setup_bindings();
    setup_layouts();

    load_config();

    setup_modules();
    setup_workspaces();

    assert(workspaces.size > 0);
    active_workspace = workspace_at(0);
}

void update_focus(struct velox_workspace * workspace)
{
    if (workspace->focus_type == TILE)
    {
        if (list_is_empty(&workspace->tiled.windows)) focus(NULL);
        else
        {
            focus(link_entry(workspace->tiled.focus, struct velox_window));
        }
    }
    else
    {
        if (list_is_empty(&workspace->floated.windows)) focus(NULL);
        else
        {
            focus(list_first(&workspace->floated.windows, struct velox_window));
        }
    }
}

void set_workspace(union velox_argument argument)
{
    uint8_t index = argument.uint8;

    DEBUG_ENTER

    assert(index < workspaces.size);

    if (workspace == workspace_at(index)) return; // Nothing to do...
    else
    {
        struct velox_window * window;

        /* Show the windows now visible */
        list_for_each_entry(&workspace_at(index)->tiled.windows, window)
            show_window(window);

        list_for_each_entry(&workspace_at(index)->floated.windows, window)
            show_window(window);

        update_focus(workspace_at(index));

        /* Hide windows no longer visible */
        list_for_each_entry(&workspace->tiled.windows, window)
            hide_window(window);

        list_for_each_entry(&workspace->floated.windows, window)
            hide_window(window);

        workspace = workspace_at(index);

        if (workspace->focus_type == TILE)
        {
            arrange();
        }

        run_hooks(workspace, VELOX_HOOK_WORKSPACE_CHANGED);
    }
}

void move_focus_to_workspace(union velox_argument argument)
{
    uint8_t index = argument.uint8;

    DEBUG_ENTER

    if (workspace->focus_type == TILE)
    {
        if (list_is_empty(&workspace->tiled.windows)) return;
        else
        {
            struct velox_link * next_focus;
            struct velox_window * window;

            window = link_entry(workspace->tiled.focus, struct velox_window);
            next_focus = list_next_link(&workspace->tiled.windows, workspace->tiled.focus);

            /* Move the focus from the old list to the new list */
            link_move_after(workspace->tiled.focus,
                &workspace_at(index)->tiled.windows.head);

            if (list_is_singular(&workspace_at(index)->tiled.windows))
            {
                /* If the workspace was empty before, set its focus to the new window */
                workspace_at(index)->tiled.focus = list_first_link
                    (&workspace_at(index)->tiled.windows);
            }

            if (list_is_empty(&workspace->tiled.windows))
            {
                next_focus = &workspace->tiled.windows.head;

                if (!list_is_empty(&workspace->floated.windows))
                {
                    /* Switch focus type to float if those are the only windows
                     * on this workspace. */
                    workspace->focus_type = FLOAT;
                }
            }

            workspace->tiled.focus = next_focus;

            update_focus(workspace);
            hide_window(window);
            arrange();

            /* If the new workspace only has tiling windows, set its focus type
             * to tile */
            if (list_is_empty(&workspace_at(index)->floated.windows))
            {
                workspace_at(index)->focus_type = TILE;
            }
        }
    }
    else if (workspace->focus_type == FLOAT)
    {
        if (list_is_empty(&workspace->floated.windows)) return;
        else
        {
            struct velox_window * window;

            window = list_first(&workspace->floated.windows,
                struct velox_window);

            list_del(window);
            list_append(&workspace_at(index)->floated.windows, window);

            /* Switch focus type to tile if those are the only windows on this
             * workspace */
            if (list_is_empty(&workspace->floated.windows))
            {
                workspace->focus_type = TILE;
            }

            update_focus(workspace);
            hide_window(window);
            arrange();

            if (list_is_empty(&workspace_at(index)->tiled.windows))
            {
                workspace_at(index)->focus_type = FLOAT;
            }
        }
    }
}

void set_focus_type(enum velox_workspace_focus_type focus_type)
{
    if (workspace->focus_type == focus_type) return;

    if (focus_type == TILE && !list_is_empty(&workspace->tiled.windows))
    {
        workspace->focus_type = focus_type;

        focus(link_entry(workspace->tiled.focus, struct velox_window));
    }
    else if (focus_type == FLOAT && !list_is_empty(&workspace->floated.windows))
    {
        workspace->focus_type = focus_type;

        focus(list_first(&workspace->floated.windows, struct velox_window));
    }
}

void next_workspace()
{
    struct velox_workspace * workspace_iterator;
    uint8_t index;

    DEBUG_ENTER

    vector_for_each_with_index(&workspaces, workspace_iterator, index)
    {
        if (workspace_iterator == active_workspace) break;
    }

    if (++index == workspaces.size)
    {
        index = 0;
    }

    set_workspace(uint8_argument(index));
}

void previous_workspace()
{
    struct velox_workspace * workspace_iterator;
    uint8_t index;

    DEBUG_ENTER

    vector_for_each_with_index(&workspaces, workspace_iterator, index)
    {
        if (workspace_iterator == active_workspace) break;
    }

    if (index-- == 0)
    {
        index = workspaces.size - 1;
    }

    set_workspace(uint8_argument(index));
}

void toggle_focus_type()
{
    if (workspace->focus_type == TILE)  set_focus_type(FLOAT);
    else                                set_focus_type(TILE);
}

void set_layout(struct velox_link * link)
{
    struct velox_layout * layout;

    workspace->layout = link;
    layout = link_entry(link, struct velox_layout_entry)->layout;
    memcpy(&workspace->state, layout->default_state,
        layout->default_state_size);

    arrange();
}

void next_layout()
{
    DEBUG_ENTER

    set_layout(list_next_link(&workspace->layouts, workspace->layout));
}

void previous_layout()
{
    DEBUG_ENTER

    set_layout(list_prev_link(&workspace->layouts, workspace->layout));
}

void focus_next()
{
    DEBUG_ENTER

    if (workspace->focus_type == TILE)
    {
        if (list_is_trivial(&workspace->tiled.windows))
        {
            return;
        }

        workspace->tiled.focus = list_next_link(&workspace->tiled.windows,
            workspace->tiled.focus);

        focus(link_entry(workspace->tiled.focus, struct velox_window));
    }
    else if (workspace->focus_type == FLOAT)
    {
        struct velox_window * window;

        if (list_is_trivial(&workspace->floated.windows))
        {
            return;
        }

        window = list_last(&workspace->floated.windows, struct velox_window);

        link_move_after(&window->link, &workspace->floated.windows.head);

        focus(window);
        restack();
    }
}

void focus_previous()
{
    DEBUG_ENTER

    if (workspace->focus_type == TILE)
    {
        if (list_is_trivial(&workspace->tiled.windows))
        {
            return;
        }

        workspace->tiled.focus = list_prev_link(&workspace->tiled.windows,
            workspace->tiled.focus);

        focus(link_entry(workspace->tiled.focus, struct velox_window));
    }
    else if (workspace->focus_type == FLOAT)
    {
        struct velox_window * window;

        if (list_is_trivial(&workspace->floated.windows))
        {
            return;
        }

        window = list_first(&workspace->floated.windows, struct velox_window);

        link_move_before(&window->link, &workspace->floated.windows.head);

        focus(window);
        restack();
    }
}

void move_next()
{
    DEBUG_ENTER

    if (workspace->focus_type == TILE)
    {
        struct velox_window * first, * second;

        if (list_is_trivial(&workspace->tiled.windows)) return;

        first = link_entry(workspace->tiled.focus, struct velox_window);
        second = link_entry(list_next_link(&workspace->tiled.windows,
            workspace->tiled.focus), struct velox_window);

        /* Swap the two windows */
        link_swap(&first->link, &second->link);

        arrange();
    }
}

void move_previous()
{
    DEBUG_ENTER

    if (workspace->focus_type == TILE)
    {
        struct velox_window * first, * second;

        if (list_is_trivial(&workspace->tiled.windows)) return;

        first = link_entry(workspace->tiled.focus, struct velox_window);
        second = link_entry(list_prev_link(&workspace->tiled.windows,
            workspace->tiled.focus), struct velox_window);

        /* Swap the two windows */
        link_swap(&first->link, &second->link);

        arrange();
    }
}

void toggle_floating()
{
    struct velox_window * window;

    if (workspace->focus_type == TILE)
    {
        if (!list_is_empty(&workspace->tiled.windows))
        {
            window = link_entry(workspace->tiled.focus, struct velox_window);
            workspace->tiled.focus = list_next_link(&workspace->tiled.windows,
                workspace->tiled.focus);

            link_move_after(&window->link, &workspace->floated.windows.head);

            window->floating = true;
            workspace->focus_type = FLOAT;

            update_focus(workspace);
            restack();
            arrange();
        }
    }
    else
    {
        if (!list_is_empty(&workspace->floated.windows))
        {
            window = list_first(&workspace->floated.windows,
                struct velox_window);

            link_move_after(&window->link, &workspace->tiled.windows.head);

            workspace->tiled.focus = &window->link;

            window->floating = false;
            workspace->focus_type = TILE;

            update_focus(workspace);
            restack();
            arrange();
        }
    }
}

void arrange()
{
    DEBUG_ENTER

    if (list_is_empty(&workspace->tiled.windows)) return;

    assert(!list_is_empty(&workspace->layouts));

    calculate_work_area(&screen_area, &work_area);
    link_entry(workspace->layout, struct velox_layout_entry)->layout->arrange
        (&work_area, &workspace->tiled.windows, &workspace->state);
}

void restack()
{
    struct velox_window * window;

    /* XXX: restack windows */
}

void spawn(char * const command[])
{
    DEBUG_ENTER

    if (fork() == 0)
    {
        setsid();
        execvp(command[0], command);
        exit(EXIT_SUCCESS);
    }
}

void catch_int(int signal)
{
    DEBUG_ENTER
    quit();
}

void catch_alarm(int signal)
{
    clock_tick_update = true;
}

void catch_chld(int signal)
{
    /* Clean up zombie processes */
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void run()
{
    struct epoll_event events[32];
    struct epoll_event event;
    int epoll_fd;
    int count;
    uint32_t index;
    sigset_t blocked_set, empty_set;
    struct itimerval timer;

    printf("\n** Main Event Loop **\n");

    /* Initialize signal masks */
    sigemptyset(&blocked_set);
    sigemptyset(&empty_set);

    sigaddset(&blocked_set, SIGALRM);
    sigprocmask(SIG_BLOCK, &blocked_set, NULL);

    /* Setup signal handlers */
    signal(SIGALRM, &catch_alarm);
    signal(SIGINT, &catch_int);
    signal(SIGCHLD, &catch_chld);

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd == -1)
        die("Could not create epoll file descriptor\n");

    /* Start timer */
    timer.it_interval.tv_usec = 0;
    timer.it_interval.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_value.tv_sec = 1;

    setitimer(ITIMER_REAL, &timer, NULL);

    /* Main event loop */
    while (running)
    {
        count = epoll_pwait(epoll_fd, events, ARRAY_LENGTH(events), -1,
            &empty_set);

        if (count == -1)
        {
            if (errno == EINTR)
            {
                if (clock_tick_update)
                {
                    clock_tick_update = false;
                    run_hooks(NULL, VELOX_HOOK_CLOCK_TICK);
                }
            }

            continue;
        }

        for (index = 0; index < count; ++index)
        {
            ((void (*)()) events[index].data.ptr)();
        }
    }
}

void quit()
{
    running = false;
}

void cleanup()
{
    cleanup_modules();
    cleanup_bindings();
    cleanup_workspaces();
    cleanup_work_area_modifiers();
    cleanup_hooks();
    cleanup_resources();
}

void __attribute__((noreturn)) die(const char * const message, ...)
{
    va_list args;

    va_start(args, message);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, message, args);
    fputc('\n', stderr);
    va_end(args);

    cleanup();

    exit(EXIT_FAILURE);
}

int main(int argc, char ** argv)
{
    srand(time(NULL));

    printf("Velox Window Manager\n");

    setup();
    run_hooks(NULL, VELOX_HOOK_STARTUP);
    run();
    cleanup();

    return EXIT_SUCCESS;
}

// vim: fdm=syntax fo=croql et sw=4 sts=4 ts=8

