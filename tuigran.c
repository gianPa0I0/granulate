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
/*
typedef enum FilterMode {
    MODE_PIXELS,
    MODE_INTERLACED_H,
    MODE_INTERLACED_V,
    MODE_DITHER
} filter_mode;

typedef enum GhostingMode {
    NO_GHOSTING,
    LUMA_GHOSTING,
    CHROMA_GHOSTING
} ghosting_mode;
*/

const static char *filter_mode[] = {
    "MODE_PIXELS", "MODE_INTERLACED_H", "MODE_INTERLACED_V", "MODE_DITHER"
};

const static char *ghosting_mode[] = {
    "NO_GHOSTING", "LUMA_GHOSTING", "CHROMA_GHOSTING"
};

#define NPARAMS 14
#define INPUT NPARAMS
#define OUTPUT (NPARAMS + 1)

typedef struct granulate_params { 
    uint64_t val;
    uint64_t min;
    uint64_t max;
    const char *param;
} granulate_params;

typedef struct windows {
    WINDOW *win_params;
    WINDOW *win_values;
    WINDOW *win_input;
    WINDOW *win_output;
    WINDOW *win_live;
} windows;

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
static int live = 0;
static int cmd = 0;
static int sel = 0;
static char input[256] = "";
static char output[256] = "";
static char compose[8192];


static void draw_screen(windows *windows) {

    werase(windows->win_params);
    werase(windows->win_values);
    werase(windows->win_input);
    werase(windows->win_output);
    werase(windows->win_live);
    box(windows->win_params, 0, 0);
    box(windows->win_values, 0, 0);
    box(windows->win_input, 0, 0);
    box(windows->win_output, 0, 0);
    box(windows->win_live, 0, 0);

    mvprintw(LINES - 2, 2, "Up and Down to move, Tab to change OUTPUT Mode, Enter to send the command, ^C to quit");
    mvwprintw(windows->win_params, 0, 2, "Params");
    mvwprintw(windows->win_values, 0, 2, "Values");
    if (sel == INPUT)
        wattron(windows->win_input, A_STANDOUT);
    mvwprintw(windows->win_input, 0, 2, "Input");
    if (sel == OUTPUT)
        wattron(windows->win_output, A_STANDOUT);
    mvwprintw(windows->win_output, 0, 2, "Output");
    wattroff(windows->win_input, A_STANDOUT);
    wattroff(windows->win_output, A_STANDOUT);

    mvwprintw(windows->win_input, 1, 2, "%s", input);
    mvwprintw(windows->win_output, 1, 2, "%s", output);
    wattroff(windows->win_input, A_STANDOUT);
    wattroff(windows->win_output, A_STANDOUT);

    if (live) {
        mvwprintw(windows->win_live, 1, 2, "%s", "LIVE");
    }
    else {
        mvwprintw(windows->win_live, 1, 2, "%s", "FILE");
    }
    
    int row_values, col_values;
    getmaxyx(windows->win_values, row_values, col_values);
    
    for (int i = 0; i < NPARAMS; i++) {
        int y = i + 1;
        if (y >= row_values - 1) break; 

        if (i == sel) {
            wattron(windows->win_params, A_STANDOUT);
            mvwprintw(windows->win_params, y, 2, "%s", params[i].param);
            wattroff(windows->win_params, A_STANDOUT);
            if (i == 0) {
                wattron(windows->win_values, A_STANDOUT);
                mvwprintw(windows->win_values, y, 2, "%s", filter_mode[params[i].val]);
                wattroff(windows->win_values, A_STANDOUT);
            }
            else if (i == 9) {
                wattron(windows->win_values, A_STANDOUT);
                mvwprintw(windows->win_values, y, 2, "%s", ghosting_mode[params[i].val]);
                wattroff(windows->win_values, A_STANDOUT);
            }
            else {
            wattron(windows->win_values, A_STANDOUT);
            mvwprintw(windows->win_values, y, 2, "%lu", params[i].val);
            wattroff(windows->win_values, A_STANDOUT);
            }
        }
        else {
            mvwprintw(windows->win_params, y, 2, "%s", params[i].param);
            if (i == 0) {
            mvwprintw(windows->win_values, y, 2, "%s", filter_mode[params[i].val]);
            }
            else if (i == 9) {
                mvwprintw(windows->win_values, y, 2, "%s", ghosting_mode[params[i].val]);
            }
            else {
            mvwprintw(windows->win_values, y, 2, "%lu", params[i].val);
            }
        }
    }

    refresh();
    wnoutrefresh(windows->win_params);
    wnoutrefresh(windows->win_values);
    wnoutrefresh(windows->win_input);
    wnoutrefresh(windows->win_output);
    wnoutrefresh(windows->win_live);
    doupdate();   
}

static void layout_windows(windows *windows) {
    if (windows->win_params) delwin(windows->win_params);
    if (windows->win_values) delwin(windows->win_values);
    if (windows->win_input)  delwin(windows->win_input);
    if (windows->win_output) delwin(windows->win_output);
    if (windows->win_live) delwin(windows->win_live);

    int top_width = COLS / 2 - 2;
    int top_height = NPARAMS + 2;

    windows->win_params = newwin(top_height, top_width, 1, 1);
    windows->win_values = newwin(top_height, top_width, 1, top_width + 2);

    int middle_height = 3;
    int middle_y = top_height + 4;
    int middle_width = 8;

    windows->win_live = newwin(middle_height, middle_width, middle_y, 1);

    int bottom_height = 4;
    int bottom_y = LINES - bottom_height - 4;
    int bottom_width = COLS / 2 - 2;

    windows->win_input  = newwin(bottom_height, bottom_width, bottom_y, 1);
    windows->win_output = newwin(bottom_height, bottom_width, bottom_y, bottom_width + 2);

}

void manage_input(int ch, windows *windows) {
    int i;
    if (ch == KEY_RESIZE) {
            clear();
            refresh();
            layout_windows(windows);
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
                sel = (sel > 0) ? sel - 1 : OUTPUT;
                break;
            case KEY_DOWN:
                sel = (sel < OUTPUT) ? sel + 1 : 0;
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
            case '\t':
                if (live)
                    live = 0;
                else
                    live = 1;
                break;
            default:
                break;
        }
    }
    if (sel <= 13) {
        int mom;
        if (newval >= 0 && newval <= 9) {
            mom = params[sel].val * 10 + newval;
            if (mom <= params[sel].max)
                params[sel].val = mom;
        }
        if (newval == -1) {
            if (params[sel].val < 10) {
                params[sel].val = params[sel].min;
            }
            else {
                mom = params[sel].val / 10;
                if (mom >= params[sel].min)
                    params[sel].val = mom;
            }
        }
    }
    else if (newval != -1) {
        if (sel == INPUT && ch >= 32 && ch <= 127) {
            i = 0;
            while(input[i] != '\0') {
                if (i < 255)
                    i++;
                else
                    break;
            }
            if (i < 255) {
                input[i] = ch;
                input[++i] = '\0';
            }
        }
        else if (sel == (OUTPUT) && ch >= 32 && ch <= 127) {
            i = 0;
            while(output[i] != '\0') {
                if (i < 255)
                    i++;
                else
                    break;
            }
            if (i < 255) {
                output[i] = ch;
                output[++i] = '\0';
            }
        }
    }
    else if (newval == -1) {
        if(sel == INPUT) {
            i = 0;
            while(input[i] != '\0') {
                if (i < 255)
                    i++;
                else
                    break;
            }
            if (i > 0) {
                input[i] = 'x';
                input[--i] = '\0';
            }
        }
        else if (sel == OUTPUT) {
            i = 0;
            while(output[i] != '\0') {
                if (i < 255)
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
    if (params[5].val || params[6].val) {
            params[7].val = 0;
    }
    else {
        params[7].val = 1;
    }
    if (cmd) {
        char *zmq;
        char *loop;
        char *force;
        if (live) {
            //zmq = "zmq,";
            zmq = "";
            loop= "-stream_loop -1 -re";
            force = "-f matroska - | cvlc -";

        }
        else {
            zmq = "";
            loop = "";
            force = "";
        }

        snprintf(compose, sizeof(compose), "./ffmpeg %s -i %s -vf \
        \"%sgranulate=%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu:%s=%lu\" %s"\
        , loop ,input, zmq, params[0].param, params[0].val, params[1].param, params[1].val, params[2].param, params[2].val\
        , params[3].param, params[3].val, params[4].param, params[4].val, params[5].param, params[5].val, params[6].param, params[6].val\
        , params[7].param, params[7].val, params[8].param, params[8].val, params[9].param, params[9].val, params[10].param, params[10].val\
        , params[11].param, params[11].val, params[12].param, params[12].val, params[13].param, params[13].val, (live ? force : output));

        run = 0;
    }
}

int main(int argc, char *argv[]) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    int ch;

    windows windows = {0};

    layout_windows(&windows);
    draw_screen(&windows);

    while (run) {
        ch = getch();
        newval = -2;
        manage_input(ch, &windows);
        draw_screen(&windows);
    }

    delwin(windows.win_params);
    delwin(windows.win_values);
    delwin(windows.win_input);
    delwin(windows.win_output);
    delwin(windows.win_live);
    endwin();
    printf("Executing Command, ^c to Halt\n");
    system(compose);
    printf("Press any button to exit\n");
    getchar();
    return 0;
}
