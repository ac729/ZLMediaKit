#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include "windows.h"
#else
#include "unistd.h"
#endif
#include "mk_mediakit.h"
#include <stdint.h>

typedef struct {
    mk_pusher pusher;
    char *url;
} Context;

void release_context(void *user_data) {
    Context *ptr = (Context *)user_data;
    if (ptr->pusher) {
        mk_pusher_release(ptr->pusher);
    }
    free(ptr->url);
    free(ptr);
    log_info("停止推流");
}

void on_push_result(void *user_data, int err_code, const char *err_msg) {
    Context *ptr = (Context *)user_data;
    if (err_code == 0) {
        log_info("推流成功: %s", ptr->url);
    } else {
        log_warn("推流%s失败: %d(%s)", ptr->url, err_code, err_msg);
    }
}

void on_push_shutdown(void *user_data, int err_code, const char *err_msg) {
    Context *ptr = (Context *)user_data;
    log_warn("推流%s中断: %d(%s)", ptr->url, err_code, err_msg);
}

void API_CALL on_regist(void *user_data, mk_media_source sender, int regist) {
    Context *ptr = (Context *)user_data;
    const char *schema = mk_media_source_get_schema(sender);
    if (strstr(ptr->url, schema) != ptr->url) {
        return;
    }

    if (!regist) {
        if (ptr->pusher) {
            mk_pusher_release(ptr->pusher);
            ptr->pusher = NULL;
        }
    } else {
        if (!ptr->pusher) {
            ptr->pusher = mk_pusher_create_src(sender);
            mk_pusher_set_on_result2(ptr->pusher, on_push_result, ptr, NULL);
            mk_pusher_set_on_shutdown2(ptr->pusher, on_push_shutdown, ptr, NULL);
            mk_pusher_publish(ptr->pusher, ptr->url);
        }
    }
}

static void fill_random_yuv420(uint8_t *y, uint8_t *u, uint8_t *v, int width, int height, int stride_y, int stride_u, int stride_v) {
    // 生成随机的整幅纯色画面
    uint8_t Y = (uint8_t)(rand() % 256);
    uint8_t U = (uint8_t)(rand() % 256);
    uint8_t V = (uint8_t)(rand() % 256);

    int i;
    for (i = 0; i < height; ++i) {
        memset(y + i * stride_y, Y, width);
    }
    int cw = width / 2;
    int ch = height / 2;
    int j;
    for (j = 0; j < ch; ++j) {
        memset(u + j * stride_u, U, cw);
        memset(v + j * stride_v, V, cw);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        log_error("Usage: rtsp_url [width height fps]");
        return -1;
    }

    int width = 640;
    int height = 360;
    int fps = 25;
    const char* url = argv[1];

    mk_config config = { .ini = NULL,
                         .ini_is_path = 1,
                         .log_level = 0,
                         .log_mask = LOG_CONSOLE,
                         .log_file_path = NULL,
                         .log_file_days = 0,
                         .ssl = NULL,
                         .ssl_is_path = 1,
                         .ssl_pwd = NULL,
                         .thread_num = 0 };
    mk_env_init(&config);

    // 创建媒体
    mk_media media = mk_media_create("__defaultVhost__", "live", "test", 0, 0, 0);

    // 初始化视频（输入YUV，内部会完成编码），必须用 mk_media_init_video 以填充 _video
    // 否则 inputYUV 会访问空的 _video 导致段错误
    mk_media_init_video(media, MKCodecH264, width, height, (float)fps, 2 * 1024 * 1024);
    mk_media_init_complete(media);

    // 推流上下文
    Context *ctx = (Context *)malloc(sizeof(Context));
    memset(ctx, 0, sizeof(Context));
    ctx->url = strdup(url);
    mk_media_set_on_regist2(media, on_regist, ctx, release_context);

    // 分配YUV420P平面缓冲
    int stride_y = width;
    int stride_u = width / 2;
    int stride_v = width / 2;
    int chroma_h = height / 2;
    size_t size_y = (size_t)stride_y * height;
    size_t size_u = (size_t)stride_u * chroma_h;
    size_t size_v = (size_t)stride_v * chroma_h;
    uint8_t *plane_y = (uint8_t *)malloc(size_y);
    uint8_t *plane_u = (uint8_t *)malloc(size_u);
    uint8_t *plane_v = (uint8_t *)malloc(size_v);
    if (!plane_y || !plane_u || !plane_v) {
        log_error("分配YUV缓冲失败");
        free(plane_y); free(plane_u); free(plane_v);
        mk_media_release(media);
        return -1;
    }


    int64_t dts_ms = 0;
    const int frame_interval_ms = 1000 / fps;

    log_info("开始生成随机YUV(%dx%d@%dfps)，通过mk_media_input_yuv推流到: %s", width, height, fps, ctx->url);

    while (1) {
        // 填充随机颜色的YUV420P并推送
        fill_random_yuv420(plane_y, plane_u, plane_v, width, height, stride_y, stride_u, stride_v);
        const char *planes[3] = { (const char *)plane_y, (const char *)plane_u, (const char *)plane_v };
        int linesize[3] = { stride_y, stride_u, stride_v };
        mk_media_input_yuv(media, planes, linesize, (uint64_t)dts_ms);

#ifdef _WIN32
        Sleep(frame_interval_ms);
#else
        usleep(frame_interval_ms * 1000);
#endif
        dts_ms += frame_interval_ms;
    }

    log_info("停止生成与推流");
    free(plane_y);
    free(plane_u);
    free(plane_v);
    mk_media_release(media);
    return 0;
}