/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "PlatformUtilities.h"
#import "TestWKWebView.h"
#import "WKWebViewConfigurationExtras.h"
#import <WebKit/WKWebViewPrivate.h>
#import <wtf/RetainPtr.h>

namespace TestWebKitAPI {

static RetainPtr<TestWKWebView> createInWindowFullscreenWebView()
{
    RetainPtr configuration = [WKWebViewConfiguration _test_configurationWithTestPlugInClassName:@"WebProcessPlugInWithInternals" configureJSCForTesting:YES];
    [configuration preferences].elementFullscreenEnabled = YES;
    [configuration setMediaTypesRequiringUserActionForPlayback:WKAudiovisualMediaTypeAudio];
    auto webView = adoptNS([[TestWKWebView alloc] initWithFrame:NSMakeRect(0, 0, 300, 300) configuration:configuration.get() addToWindow:YES]);
    return webView;
}

TEST(InWindowFullscreen, EmptyDocument)
{
    auto webView = createInWindowFullscreenWebView();
    [webView synchronouslyLoadHTMLString:@"<html></html>"];
    EXPECT_FALSE([webView _canToggleInWindow]);
    EXPECT_FALSE([webView _isInWindowActive]);

    [webView _close];
}

// rdar://136551692
#if PLATFORM(MAC)
TEST(InWindowFullscreen, DISABLED_CanToggleAfterPlaybackStarts)
#else
TEST(InWindowFullscreen, CanToggleAfterPlaybackStarts)
#endif
{
    auto webView = createInWindowFullscreenWebView();
    [webView synchronouslyLoadTestPageNamed:@"video-with-audio"];

    bool isPlaying = false;
    [webView performAfterReceivingMessage:@"playing" action:[&] { isPlaying = true; }];
    [webView objectByEvaluatingJavaScriptWithUserGesture:@"go()"];
    TestWebKitAPI::Util::run(&isPlaying);

    [webView _updateMediaPlaybackControlsManager];
    TestWebKitAPI::Util::waitFor([&] {
        [webView _updateMediaPlaybackControlsManager];
        return [webView _canToggleInWindow];
    });
    EXPECT_TRUE([webView _canToggleInWindow]);

    [webView _close];
}

// rdar://136551692
#if PLATFORM(MAC)
TEST(InWindowFullscreen, DISABLED_ToggleChangesIsActive)
#else
TEST(InWindowFullscreen, ToggleChangesIsActive)
#endif
{
    auto webView = createInWindowFullscreenWebView();
    [webView synchronouslyLoadTestPageNamed:@"video-with-audio"];

    bool isPlaying = false;
    [webView performAfterReceivingMessage:@"playing" action:[&] { isPlaying = true; }];
    [webView objectByEvaluatingJavaScriptWithUserGesture:@"go()"];
    TestWebKitAPI::Util::run(&isPlaying);

    TestWebKitAPI::Util::waitFor([&] {
        [webView _updateMediaPlaybackControlsManager];
        return [webView _canToggleInWindow];
    });
    EXPECT_TRUE([webView _canToggleInWindow]);

    [webView _toggleInWindow];
    TestWebKitAPI::Util::waitFor([&] {
        return [webView _isInWindowActive];
    });
    EXPECT_TRUE([webView _isInWindowActive]);

    [webView _toggleInWindow];
    TestWebKitAPI::Util::waitFor([&] {
        return ![webView _isInWindowActive];
    });
    EXPECT_FALSE([webView _isInWindowActive]);

    [webView _close];
}

// rdar://136551692
#if PLATFORM(MAC)
TEST(InWindowFullscreen, DISABLED_EnterAndExitChangeIsActive)
#else
TEST(InWindowFullscreen, EnterAndExitChangeIsActive)
#endif
{
    auto webView = createInWindowFullscreenWebView();
    [webView synchronouslyLoadTestPageNamed:@"video-with-audio"];

    bool isPlaying = false;
    [webView performAfterReceivingMessage:@"playing" action:[&] { isPlaying = true; }];
    [webView objectByEvaluatingJavaScriptWithUserGesture:@"go()"];
    TestWebKitAPI::Util::run(&isPlaying);

    TestWebKitAPI::Util::waitFor([&] {
        [webView _updateMediaPlaybackControlsManager];
        return [webView _canToggleInWindow];
    });
    EXPECT_TRUE([webView _canToggleInWindow]);

    [webView _enterInWindow];
    TestWebKitAPI::Util::waitFor([&] {
        return [webView _isInWindowActive];
    });
    EXPECT_TRUE([webView _isInWindowActive]);

    [webView _exitInWindow];
    TestWebKitAPI::Util::waitFor([&] {
        return ![webView _isInWindowActive];
    });
    EXPECT_FALSE([webView _isInWindowActive]);

    [webView _close];
}

// rdar://136551692
#if PLATFORM(MAC)
TEST(InWindowFullscreen, DISABLED_EnterChangesIsActiveWithoutUserGesture)
#else
TEST(InWindowFullscreen, EnterChangesIsActiveWithoutUserGesture)
#endif
{
    auto webView = createInWindowFullscreenWebView();
    [webView synchronouslyLoadTestPageNamed:@"video-with-audio"];

    bool isPlaying = false;
    [webView performAfterReceivingMessage:@"playing" action:[&] { isPlaying = true; }];
    [webView objectByEvaluatingJavaScriptWithUserGesture:@"go()"];
    TestWebKitAPI::Util::run(&isPlaying);

    TestWebKitAPI::Util::waitFor([&] {
        [webView _updateMediaPlaybackControlsManager];
        return [webView _canToggleInWindow];
    });
    EXPECT_TRUE([webView _canToggleInWindow]);

    [webView objectByEvaluatingJavaScriptWithUserGesture:@"internals.consumeTransientActivation()"];

    [webView _enterInWindow];
    TestWebKitAPI::Util::waitFor([&] {
        return [webView _isInWindowActive];
    });
    EXPECT_TRUE([webView _isInWindowActive]);
}

} // namespace TestWebKitAPI
