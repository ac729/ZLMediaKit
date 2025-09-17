/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ENABLE_MP4
#define ENABLE_MP4
#endif

#ifdef ENABLE_MP4
#include "Common/Device.h"
#include "Common/Parser.h"
#include "Common/config.h"
#include "Poller/EventPoller.h"
#include "Pusher/MediaPusher.h"
#include "Record/MP4Reader.h"
#include "Util/logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <signal.h>
#include <thread>

using namespace std;
using namespace toolkit;
using namespace mediakit;

// 推流器，保持强引用
MediaPusher::Ptr pusher;

void createPusher(const EventPoller::Ptr &poller, const string &schema, const string &vhost, const string &app, const string &stream, const string &url) {
    // 创建推流器并绑定一个MediaSource  [AUTO-TRANSLATED:b0721d46]
    // Create a streamer and bind a MediaSource
    pusher.reset(new MediaPusher(schema, vhost, app, stream, poller));
    // 可以指定rtsp推流方式，支持tcp和udp方式，默认tcp
    //(*pusher)[Client::kRtpType] = Rtsp::RTP_UDP;
    // 设置推流中断处理逻辑  [AUTO-TRANSLATED:aa6c0405]

    pusher->setOnShutdown(
        [poller, schema, vhost, app, stream, url](const SockException &ex) { WarnL << "Server connection is closed:" << ex.getErrCode() << " " << ex.what(); });

    // 设置发布结果处理逻辑
    pusher->setOnPublished([poller, schema, vhost, app, stream, url](const SockException &ex) {
        if (ex) {
            WarnL << "Publish fail:" << ex.getErrCode() << " " << ex.what();
        } else {
            InfoL << "Publish success,Please play with player:" << url;
        }
    });
    pusher->publish(url);
}

// 这里才是真正执行main函数，你可以把函数名(domain)改成main，然后就可以输入自定义url了
int domain(const string &url) {
    // 设置日志  [AUTO-TRANSLATED:50ba02ba]
    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // 关闭所有转协议  [AUTO-TRANSLATED:2a58bc8f]
    // Close all protocol conversions
    mINI::Instance()[Protocol::kEnableMP4] = 0;
    mINI::Instance()[Protocol::kEnableFMP4] = 0;
    mINI::Instance()[Protocol::kEnableHls] = 0;
    mINI::Instance()[Protocol::kEnableHlsFmp4] = 0;
    mINI::Instance()[Protocol::kEnableTS] = 0;
    mINI::Instance()[Protocol::kEnableRtsp] = 0;
    mINI::Instance()[Protocol::kEnableRtmp] = 0;

    // 根据url获取媒体协议类型，注意大小写  [AUTO-TRANSLATED:3cd6622a]
    // Get the media protocol type based on the URL, note the case
    auto schema = strToLower(findSubString(url.data(), nullptr, "://").substr(0, 4));

    // 只开启推流协议对应的转协议  [AUTO-TRANSLATED:1c4975ae]
    // Only enable the protocol conversion corresponding to the push protocol
    mINI::Instance()["protocol.enable_" + schema] = 1;

    // 从mp4文件加载生成MediaSource对象  [AUTO-TRANSLATED:5e5b04ca]
    // Load the MediaSource object from the mp4 file
    auto tuple = MediaTuple { DEFAULT_VHOST, "live", "stream", "" };
    auto dev_channel = std::make_shared<DevChannel>(tuple);
    mediakit::VideoInfo zl_vinfo;
    zl_vinfo.iFrameRate = 15;
    zl_vinfo.iHeight = 1080;
    zl_vinfo.iWidth = 1920;
    dev_channel->initVideo(zl_vinfo);

    // 1) 注册媒体源注册事件监听 -> 在 addTrackCompleted 触发注册后创建推流器
    auto poller = EventPollerPool::Instance().getPoller();
    NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaChanged, [url, poller](const bool &bRegist, MediaSource &sender) {
        if (bRegist) {
            auto tuple = sender.getMediaTuple();
            std::cout << "Creating pusher for: " << sender.getSchema() << "://" << tuple.vhost << "/" << tuple.app << "/" << tuple.stream << std::endl;
            createPusher(poller, sender.getSchema(), tuple.vhost, tuple.app, tuple.stream, url);
        }
    });

    // 2) 标记轨道添加完成(触发 MediaSource 注册广播)
    dev_channel->addTrackCompleted();

    int width = zl_vinfo.iWidth;
    int height = zl_vinfo.iHeight;
    int y_stride = width;
    int uv_stride = width / 2;

    std::shared_ptr<std::vector<unsigned char>> y_plane = std::make_shared<std::vector<unsigned char>>(width * height);
    std::shared_ptr<std::vector<unsigned char>> u_plane = std::make_shared<std::vector<unsigned char>>(uv_stride * (height / 2));
    std::shared_ptr<std::vector<unsigned char>> v_plane = std::make_shared<std::vector<unsigned char>>(uv_stride * (height / 2));

    // 15fps -> 每帧约 66.666 ms
    int fps = 15;
    int frame_interval_ms = 1000 / fps;

    // 3) 启动帧生产线程：循环生成随机纯色帧并推送 (15fps)
    std::thread([dev_channel, y_plane, u_plane, v_plane, width, height, y_stride, uv_stride, frame_interval_ms]() {
        std::mt19937 rng { std::random_device {}() };
        std::uniform_int_distribution<int> dist(0, 255);
        uint64_t pts = 0;
        while (true) {
            int R = dist(rng);
            int G = dist(rng);
            int B = dist(rng);
            // 计算YUV(全帧统一颜色) BT.601
            double Yf = 0.299 * R + 0.587 * G + 0.114 * B;
            double Uf = -0.169 * R - 0.331 * G + 0.5 * B + 128.0;
            double Vf = 0.5 * R - 0.419 * G - 0.081 * B + 128.0;
            auto clamp255 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
            unsigned char Y = (unsigned char)clamp255((int)(Yf + 0.5));
            unsigned char U = (unsigned char)clamp255((int)(Uf + 0.5));
            unsigned char V = (unsigned char)clamp255((int)(Vf + 0.5));

            std::fill(y_plane->begin(), y_plane->end(), Y);
            std::fill(u_plane->begin(), u_plane->end(), U);
            std::fill(v_plane->begin(), v_plane->end(), V);

            char *planes[3];
            int linesize[3];
            planes[0] = (char *)y_plane->data();
            planes[1] = (char *)u_plane->data();
            planes[2] = (char *)v_plane->data();
            linesize[0] = y_stride;
            linesize[1] = uv_stride;
            linesize[2] = uv_stride;

            dev_channel->inputYUV(planes, linesize, pts);
            pts += frame_interval_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms));
        }
    }).detach();

    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); }); // 设置退出信号
    pusher.reset();
    sem.wait();
    return 0;
}

int main(int argc, char *argv[]) {
    // 推流url支持rtsp和rtmp
    return domain("rtsp://127.0.0.1:8851/live/rtsp_push");
}
#endif