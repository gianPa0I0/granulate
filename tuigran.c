#include <signal.h>
#include <ncurses.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t run = 1;

static void handler(int errno) {
    run = 0;
}

typedef enum FilterMode {
    MODE_PIXELS,
    MODE_INTERLACED_H,
    MODE_INTERLACED_V,
    MODE_DITHER
} filter_mode;

#define NPARAMS 14

typedef struct granulate_params { 
    uint64_t val;
    uint64_t min;
    uint64_t max;
    const char *param;
} granulate_params;

static granulate_params params[NPARAMS] = {
    {0, 0, 3, "mode"},
    {1, 1, 256, "zoom"},
    {0, 0, UINT64_MAX, "zoom_offset_time"},
    {0, 0, UINT64_MAX, "n_grains"},
    {1, 1, 8192, "buffer"},
    {0, 0, 8192, "grain_w"},
    {0, 0, 8192, "grain_h"},
    {1, 0, 1, "fullscreen"},
    {0, 0, 1, "var_size"},
    {0, 0, 2, "ghosting"},
    {0, 0, 1, "static_grains"},
    {0, 0, UINT64_MAX, "grains_reset_time"},
    {0, 0, 8192, "delay"},
    {0, 0, UINT32_MAX, "seed"}
};

static int newval = -2;
static int i_or_o = 0;
static int cmd = 0;
static char input[256] = "";
static char output[256] = "";
static char compose[8192];

void draw_screen(int sel, WINDOW *win_params, WINDOW *win_values, WINDOW *win_input, WINDOW *win_output) {

    werase(win_params);
    werase(win_values);
    werase(win_input);
    werase(win_output);
    box(win_params, 0, 0);
    box(win_values, 0, 0);
    box(win_input, 0, 0);
    box(win_output, 0, 0);
    mvprintw(LINES - 2, 2, "Up and Down to move, Enter to send the command, ^C to quit");
    mvwprintw(win_params, 0, 2, "Params");
    mvwprintw(win_values, 0, 2, "Values");
    if (sel == NPARAMS)
        wattron(win_input, A_STANDOUT);
    mvwprintw(win_input, 0, 2, "Input");
    if (sel == NPARAMS + 1)
        wattron(win_output, A_STANDOUT);
    mvwprintw(win_output, 0, 2, "Output");
    wattroff(win_input, A_STANDOUT);
    wattroff(win_output, A_STANDOUT);

    mvwprintw(win_input, 1, 2, "%s", input);
    mvwprintw(win_output, 1, 2, "%s", output);
    wattroff(win_input, A_STANDOUT);
    wattroff(win_output, A_STANDOUT);
    
    int row_values, col_values;
    getmaxyx(win_values, row_values, col_values);
    
    for (int i = 0; i < NPARAMS; i++) {
        int y = i + 1;
        if (y >= row_values - 1) break; 

        if (i == sel) {
            wattron(win_params, A_STANDOUT);
            mvwprintw(win_params, y, 2, "%s", params[i].param);
            wattroff(win_params, A_STANDOUT);
            
            wattron(win_values, A_STANDOUT);
            mvwprintw(win_values, y, 2, "%lu", params[i].val);
            wattroff(win_values, A_STANDOUT);
        } else {
            mvwprintw(win_params, y, 2, "%s", params[i].param);
            mvwprintw(win_values, y, 2, "%lu", params[i].val);
        }
    }

    refresh();
    wnoutrefresh(win_params);
    wnoutrefresh(win_values);
    wnoutrefresh(win_input);
    wnoutrefresh(win_output);
    doupdate();   
}

void layout_windows(WINDOW **win_params, WINDOW **win_values, WINDOW **win_input, WINDOW **win_output) {
    if (*win_params) delwin(*win_params);
    if (*win_values) delwin(*win_values);
    if (*win_input)  delwin(*win_input);
    if (*win_output) delwin(*win_output);

    int top_width = COLS / 2 - 2;
    int top_height = NPARAMS + 2;

    *win_params = newwin(top_height, top_width, 1, 1);
    *win_values = newwin(top_height, top_width, 1, top_width + 2);

    int bottom_height = 4;
    int bottom_y = LINES - bottom_height - 4;
    int bottom_width = COLS / 2 - 2;

    *win_input  = newwin(bottom_height, bottom_width, bottom_y, 1);
    *win_output = newwin(bottom_height, bottom_width, bottom_y, bottom_width + 2);
}

int main(int argc, char *argv[]) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    int i;

    WINDOW *win_params = NULL, *win_values = NULL, *win_input = NULL, *win_output = NULL;
    
    layout_windows(&win_params, &win_values, &win_input, &win_output);

    int sel = 0;
    int ch;
    draw_screen(sel, win_params, win_values, win_input, win_output);

    while (run) {
        ch = getch();
        newval = -2;
        if (ch == KEY_RESIZE) {
            clear();
            refresh();
            layout_windows(&win_params, &win_values, &win_input, &win_output);
        }
        else if (ch >= '0' && ch <='9' && sel <= NPARAMS -1)
        {
            newval = ch - '0';
        }
        else if (ch == KEY_BACKSPACE)
            {
                newval = -1;
            }
        else {
            switch (ch) {
                case KEY_UP:
                    sel = (sel > 0) ? sel - 1 : NPARAMS + 1;
                    break;
                case KEY_DOWN:
                    sel = (sel < NPARAMS + 1) ? sel + 1 : 0;
                    break;
                case KEY_RIGHT:
                    (params[sel].val < params[sel].max) ? params[sel].val++ : (params[sel].val = params[sel].min);
                    break;
                case KEY_LEFT:
                    (params[sel].val > params[sel].min) ? params[sel].val-- : (params[sel].val = params[sel].max);
                    break;
                case KEY_ENTER:
                case '\n':
                case '\r':
                    cmd = 1;
                    break;
                default:
                    break;
            }
        }
        if (sel < 13) {
            int mom;
            if (newval >= 0 && newval <= 9) {
                mom = params[sel].val * 10 + newval;
                if (mom <= params[sel].max)
                    params[sel].val = mom;
            }
            if (newval == -1) {
                mom = params[sel].val / 10;
                if (mom >= params[sel].min)
                    params[sel].val = mom;
            }
        }
        else if (newval != -1) {
            if (sel == NPARAMS && ch >= 32 && ch <= 127) {
                i = 0;
                while(input[i] != '\0') {
                    if (i < 256)
                        i++;
                    else
                        break;
                }
                if (i < 256) {
                    input[i] = ch;
                    input[++i] = '\0';
                }
            }
            else if (sel == (NPARAMS + 1) && ch >= 32 && ch <= 127) {
                i = 0;
                while(output[i] != '\0') {
                    if (i < 256)
                        i++;
                    else
                        break;
                }
                if (i < 256) {
                    output[i] = ch;
                    output[++i] = '\0';
                }
            }
        }
        else if (newval == -1) {
            if(sel == NPARAMS) {
                i = 0;
                while(input[i] != '\0') {
                    if (i < 256)
                        i++;
                    else
                        break;
                }
                if (i > 0) {
                    input[i] = 'x';
                    input[--i] = '\0';
                }
            }
            else if (sel == NPARAMS + 1) {
                i = 0;
                while(output[i] != '\0') {
                    if (i < 256)
                        i++;
                    else
                        break;
                }
                if (i > 0) {
                    output[i] = 'x';
                    output[--i] = '\0';
                }
            }
        }
        draw_screen(sel, win_params, win_values, win_input, win_output);
        if (cmd) {

            snprintf(compose, sizeof(compose), "./ffmpeg -i %s -vf \
            \"granulate=%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu\" %s"\
            , input, params[0].param, params[0].val, params[1].param, params[1].val, params[2].param, params[2].val\
            , params[3].param, params[3].val, params[4].param, params[4].val, params[5].param, params[5].val, params[6].param, params[6].val\
            , params[7].param, params[7].val, params[8].param, params[8].val, params[9].param, params[9].val, params[10].param, params[10].val\
            , params[11].param, params[11].val, params[12].param, params[12].val, params[13].param, params[13].val, output);

            run = 0;
        }
    }

    delwin(win_params);
    delwin(win_values);
    delwin(win_input);
    delwin(win_output);
    endwin();
    printf("Executing Command\n");
    system(compose);
    printf("Press any button to exit\n");
    getchar();
    return 0;
}
