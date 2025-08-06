#include<stdio.h>
#include<stdlib.h>
#include<time.h>
#include<string.h>
#include<math.h>
#include<unistd.h>

#include<xcb/xcb.h>
#include <xcb/xcb_image.h>
#include <xcb/xproto.h>

#include <signal.h>

int SIZEX;
int SIZEY;

xcb_connection_t *con = NULL;

// одна текстура перлина едет влево а другая вправо и в результате эффект воды - сдвигаем пиксели на экране

typedef struct {
    float x; float y;
} gradientVec;

int random_int;
char *permutation_table;
uint32_t *left_noise, *right_noise;
uint32_t *total_noise; // max(left_noise, right_noise)
xcb_image_t *waved_image;

xcb_grab_keyboard_reply_t *grab_reply;


void init_perlin(){
    free(permutation_table);
    permutation_table = (char*)malloc(1024);
    for(int i=0; i<1024;i++){permutation_table[i] = rand() % 255;}

}


gradientVec getPseudoRandomGradientVector(int x, int y){
    int V = (int)(((x * 1836311903) ^ (y * 2971215073) + 4807526976) & 1023);
    V = permutation_table[V]&3;
    gradientVec v;
    v.x = 0; v.y = -1;
    switch(V){
        case 0: v.x=1;v.y=0;break;
        case 1: v.x=-1;v.y=0;break;
        case 2: v.x=0;v.y=1;break;
        default: v.x = 0; v.y = -1;break;
    }
    return v;
}

static float qunticCurve(float t){
    return t * t * t * (t * (t * 6 - 15) + 10);
}

static float lerp(float a, float b, float t){
    return a + (b - a) * t;
}

static float dot(gradientVec a, gradientVec b){
    return a.x * b.x + a.y * b.y;
}

float noise(float fx, float fy){
    int left = (int)fx;
    int top = (int)fy;
    float pointInQuadX = fx - left;
    float pointInQuadY = fy - top;

    gradientVec topLeftGradient = getPseudoRandomGradientVector(left, top);
    gradientVec topRightGradient = getPseudoRandomGradientVector(left+1, top);
    gradientVec bottomLeftGradient = getPseudoRandomGradientVector(left, top+1);
    gradientVec bottomRightGradient = getPseudoRandomGradientVector(left+1, top+1);
    gradientVec distanceToTopLeft = {pointInQuadX, pointInQuadY};
    gradientVec distanceToTopRight = {pointInQuadX-1, pointInQuadY};
    gradientVec distanceToBottomLeft = {pointInQuadX, pointInQuadY-1};
    gradientVec distanceToBottomRight = {pointInQuadX-1, pointInQuadY-1};
    float tx1 = dot(distanceToTopLeft, topLeftGradient);
    float tx2 = dot(distanceToTopRight, topRightGradient);
    float bx1 = dot(distanceToBottomLeft, bottomLeftGradient);
    float bx2 = dot(distanceToBottomRight, bottomRightGradient);

    pointInQuadX = qunticCurve(pointInQuadX);
    pointInQuadY = qunticCurve(pointInQuadY);

    float tx = lerp(tx1, tx2, pointInQuadX);
    float bx = lerp(bx1, bx2, pointInQuadX);
    float tb = lerp(tx, bx, pointInQuadY);

    return tb;
}
//persistence = 0.5f
float noise_octaves(float fx, float fy, int octaves, float persistence){
    float amplitude=1;
    float max=0;
    float res=0;

    while (octaves){
        max += amplitude;
        res += noise(fx, fy) * amplitude;
        amplitude *= persistence;
        fx *= 2;
        fy *= 2;
        octaves--;
    }

    return res / max;
}



void move_right(float time) // for left_noise only
{
    // Сдвигаем все пиксели влево на одну позицию
    for(int y = 0; y < SIZEY; y++) {
        for(int x = 0; x < SIZEX - 1; x++) {
            left_noise[y * SIZEX + x] = left_noise[y * SIZEX + x + 1];
        }
    }

    // Генерируем новый шум для последнего столбца
    for(int y = 0; y < SIZEY; y++) {
        float nx = SIZEX;  // Используем SIZEX вместо SIZEX-1 для непрерывности
        float ny = y;
        float scale = 0.009;

        float n = noise_octaves((nx + time) * scale, (ny) * scale, 4, 0.5);

        uint8_t intensity = (uint8_t)((n + 1.0f) * 127.5f);
        uint32_t color = (intensity << 16) | (intensity << 8) | intensity;
        left_noise[y * SIZEX + (SIZEX - 1)] = color;  // Корректный индекс последнего элемента
    }
}

void move_left(float time) // for right_noise only
{
    // Сдвигаем все пиксели вправо на одну позицию
    for(int y = 0; y < SIZEY; y++) {
        for(int x = SIZEX - 1; x > 0; x--) {
            right_noise[y * SIZEX + x] = right_noise[y * SIZEX + x - 1];
        }
    }

    // Генерируем новый шум для первого столбца
    for(int y = 0; y < SIZEY; y++) {
        float nx = 0;  // Первый столбец (x=0)
        float ny = y;
        float scale = 0.009f;

        float n = noise_octaves((time-nx) * scale, (ny) * scale, 4, 0.5f);

        uint8_t intensity = (uint8_t)((n + 1.0f) * 127.5f);
        uint32_t color = (intensity << 16) | (intensity << 8) | intensity;
        right_noise[y * SIZEX] = color;  // Первый элемент в строке (x=0)
    }
}

void combine_noises(){
    for(int y=0; y<SIZEY; y++){
        for(int x=0; x<SIZEX;x++){
            total_noise[y*SIZEX + x] = (left_noise[y*SIZEX + x] + right_noise[y*SIZEX + x]);
        }
    }

}

void gen_waved_image(xcb_image_t* screenshot) {
    uint8_t *src_data = screenshot->data;
    uint32_t *dst_data = (uint32_t*)waved_image->data;
    int stride = screenshot->stride; 

    for (int y = 0; y < SIZEY; y++) {
        uint32_t *src_row = (uint32_t*)(src_data + y * stride);
        uint32_t *dst_row = &dst_data[y * SIZEX];

        for (int x = 0; x < SIZEX; x++) {
            uint32_t noise_value = total_noise[y * SIZEX + x] & 0xFF;
            int offset = (noise_value * 10) / 255;
            int new_y = y - offset;
            if (new_y < 0) new_y = 0;

            uint32_t *src_pixel = (uint32_t*)(src_data + new_y * stride) + x;
            dst_row[x] = *src_pixel;
        }
    }
}

void handler(int sig) {
    free(left_noise);

    xcb_disconnect(con);

    free(grab_reply);
    
    exit(EXIT_SUCCESS); 
}



int main(){
    srand((unsigned)time(NULL));

    init_perlin();
    long time = 0;

    con = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(con)){
        fprintf(stderr, "xcb error\n");
        return 0;
    }
    const xcb_setup_t *setup = xcb_get_setup(con);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t *screen = iter.data;
    SIZEX = screen->width_in_pixels;
    SIZEY = screen->height_in_pixels;
    xcb_window_t window = xcb_generate_id(con);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {screen->white_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS};

    //generating noise image
    left_noise = (uint32_t*)malloc(SIZEX * SIZEY * sizeof(uint32_t));
    total_noise = (uint32_t*)malloc(SIZEX * SIZEY * sizeof(uint32_t));

    for(int y=0; y<SIZEY; y++){
        for(int x=0; x<SIZEX; x++){
            float nx = x;
            float ny = y;
            float scale = 0.009;

            float n = noise_octaves((nx - time ) * scale, (ny) * scale, 4, 0.5);

            uint8_t intencity = (uint8_t)((n + 1.0f) * 127.5f);
            uint32_t color = (intencity << 16) | (intencity << 8) | intencity;
            left_noise[y*SIZEX+x] = color;
        }
    }
    //init_perlin();

    right_noise = (uint32_t*)malloc(SIZEX * SIZEY * sizeof(uint32_t));

    for(int y=0; y<SIZEY; y++){
        for(int x=0; x<SIZEX; x++){
            float nx = x;
            float ny = y;
            float scale = 0.009;

            float n = noise_octaves((nx) * scale, (ny) * scale, 4, 0.5);

            uint8_t intencity = (uint8_t)((n + 1.0f) * 127.5f);
            uint32_t color = (intencity << 16) | (intencity << 8) | intencity;
            right_noise[y*SIZEX+x] = color;
        }
    }

    xcb_image_t *screenshot = xcb_image_get(
        con, screen->root, 0, 0,
        SIZEX, SIZEY, ~0, XCB_IMAGE_FORMAT_Z_PIXMAP
    );

    xcb_create_window(con, 
                     XCB_COPY_FROM_PARENT,
                     window, 
                     screen->root,
                     0, 0, 
                     SIZEX, SIZEY, 
                     1,
                     XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual,
                     mask, values);
    xcb_gcontext_t gc = xcb_generate_id(con);
    uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t gc_values[2] = {screen->black_pixel, 0};

    xcb_create_gc(con, gc, window, gc_mask, gc_values);

    combine_noises();


    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(con, 1, 12,
    "WM_PROTOCOLS");
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(con, cookie, 0);

    xcb_change_property(
        con, XCB_PROP_MODE_REPLACE,
        window, XCB_ATOM_ATOM,
        XCB_ATOM_ATOM, 32, 1,
        &reply->atom
    );
    uint32_t override_values[] = { 1 };
    xcb_change_window_attributes(
        con,
        window,
        XCB_CW_OVERRIDE_REDIRECT,
        override_values
    );

    xcb_map_window(con, window);

    grab_reply = xcb_grab_keyboard_reply(
        con,
        xcb_grab_keyboard(con, 1, window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
        NULL
        );


    xcb_flush(con);

    signal(SIGINT, handler);  
    signal(SIGTERM, handler);
    signal(SIGHUP, handler);

 
    waved_image = xcb_image_create_native(con, SIZEX, SIZEY, XCB_IMAGE_FORMAT_Z_PIXMAP, screen->root_depth, NULL, 1024 * 1024 * 4, NULL);

    int run =1;
    int timer = 0;
    xcb_generic_event_t *event;

    while(run){
    while ((event = xcb_poll_for_event(con))) {
        switch (event->response_type & ~0x80) {
            case XCB_KEY_PRESS: printf("exit\n"); handler(0);
            
            case XCB_CLIENT_MESSAGE: run = 0; handler(0);

        }
        free(event);

        }
        timer++;
        if (timer%10000==0){
        time+=1;
        move_right(time);
        move_left(time);
        combine_noises();
        gen_waved_image(screenshot);
        xcb_image_put(
            con, window, gc,
            waved_image, 0, 0, 0
        );
        xcb_flush(con);
        timer=0;
        }


    }

    handler(0);
}

