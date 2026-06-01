/*
 * casc_ui_mac.m — macOS WebView GUI backend (WKWebView)
 *
 * Implements the casc_ui_backend_* hooks declared in casc_internal.h using a
 * WKWebView. The view is added as a subview of the host-provided NSView*
 * (the DAW's plugin window), or a standalone NSWindow when parent is NULL.
 *
 * UI -> DSP: a WKScriptMessageHandler named "casc" receives {id, value} from
 *            window.webkit.messageHandlers.casc.postMessage(...). The bootstrap
 *            JS (built in casc_ui.c) wires window.casc.setParam to it.
 * DSP -> UI: casc_ui_backend_eval_js() calls -evaluateJavaScript:.
 *
 * Build: compiled only on Apple. Requires the WebKit + Cocoa frameworks.
 */

#include "casc_internal.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

/* -------------------------------------------------------------------------- */
/*  Message handler: receives setParam messages from the page                 */
/* -------------------------------------------------------------------------- */

@interface CascScriptHandler : NSObject <WKScriptMessageHandler>
@property (nonatomic, assign) casc_ui_bridge_t* bridge;
@end

@implementation CascScriptHandler
- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message {
    (void)ucc;
    if (!self.bridge) return;
    id body = message.body;
    if (![body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary* d = (NSDictionary*)body;
    id pid = d[@"id"];
    id val = d[@"value"];
    if (![pid isKindOfClass:[NSNumber class]] ||
        ![val isKindOfClass:[NSNumber class]]) return;
    casc_ui_on_set_param(self.bridge, [pid intValue], [val doubleValue]);
}
@end

/* -------------------------------------------------------------------------- */
/*  Per-GUI Objective-C state (held behind the opaque void* handle)           */
/* -------------------------------------------------------------------------- */

typedef struct {
    WKWebView*         webview;
    NSWindow*          window;       /* non-nil only for standalone mode */
    CascScriptHandler* handler;
} casc_ui_mac_t;

/* -------------------------------------------------------------------------- */
/*  Backend hooks                                                             */
/* -------------------------------------------------------------------------- */

casc_error_t casc_ui_backend_open(casc_instance_t* inst, void* parent_handle,
                                   const char* html, size_t html_len,
                                   const char* bootstrap_js,
                                   casc_ui_bridge_t* bridge,
                                   int width, int height, bool resizable,
                                   void** out_handle) {
    (void)inst; (void)html_len;
    if (!out_handle) return CASC_ERR_INVALID_ARG;
    *out_handle = NULL;

    @autoreleasepool {
        casc_ui_mac_t* st = (casc_ui_mac_t*)calloc(1, sizeof(*st));
        if (!st) return CASC_ERR_OUT_OF_MEMORY;

        WKWebViewConfiguration* cfg = [[WKWebViewConfiguration alloc] init];
        WKUserContentController* ucc = [[WKUserContentController alloc] init];

        /* Inject the bootstrap shim before any page script runs. */
        if (bootstrap_js) {
            NSString* boot = [NSString stringWithUTF8String:bootstrap_js];
            WKUserScript* script =
                [[WKUserScript alloc] initWithSource:boot
                                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                    forMainFrameOnly:YES];
            [ucc addUserScript:script];
        }

        CascScriptHandler* handler = [[CascScriptHandler alloc] init];
        handler.bridge = bridge;
        [ucc addScriptMessageHandler:handler name:@"casc"];
        cfg.userContentController = ucc;
        st->handler = handler;

        NSRect frame = NSMakeRect(0, 0, width, height);
        WKWebView* webview = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
        webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        st->webview = webview;

        /* Load the HTML. baseURL nil keeps it sandboxed (no file access). */
        NSString* htmlStr = html ? [NSString stringWithUTF8String:html] : @"";
        [webview loadHTMLString:htmlStr baseURL:nil];

        if (parent_handle) {
            /* Embed into the DAW-provided NSView. */
            NSView* parent = (__bridge NSView*)parent_handle;
            webview.frame = parent.bounds;
            [parent addSubview:webview];
        } else {
            /* Standalone window (useful for tooling / standalone hosts). */
            NSUInteger styleMask = NSWindowStyleMaskTitled |
                                   NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskMiniaturizable;
            if (resizable) styleMask |= NSWindowStyleMaskResizable;
            NSWindow* win =
                [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:styleMask
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
            [win setReleasedWhenClosed:NO];
            [win setContentView:webview];
            [win center];
            [win makeKeyAndOrderFront:nil];
            st->window = win;
        }

        *out_handle = st;
        return CASC_OK;
    }
}

void casc_ui_backend_close(void* handle) {
    if (!handle) return;
    @autoreleasepool {
        casc_ui_mac_t* st = (casc_ui_mac_t*)handle;
        if (st->webview) {
            [st->webview.configuration.userContentController
                removeScriptMessageHandlerForName:@"casc"];
            [st->webview removeFromSuperview];
            st->webview = nil;
        }
        if (st->window) {
            [st->window close];
            st->window = nil;
        }
        st->handler = nil;
        free(st);
    }
}

void casc_ui_backend_tick(void* handle) {
    /* WKWebView drives itself on the main run loop; nothing to pump here.
     * Kept so hosts on a manual loop can still call it harmlessly. */
    (void)handle;
}

void casc_ui_backend_eval_js(void* handle, const char* js) {
    if (!handle || !js) return;
    @autoreleasepool {
        casc_ui_mac_t* st = (casc_ui_mac_t*)handle;
        if (!st->webview) return;
        NSString* code = [NSString stringWithUTF8String:js];
        WKWebView* wv = st->webview;
        /* Must run on the main thread. */
        if ([NSThread isMainThread]) {
            [wv evaluateJavaScript:code completionHandler:nil];
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{
                [wv evaluateJavaScript:code completionHandler:nil];
            });
        }
    }
}

casc_error_t casc_ui_backend_set_size(void* handle, int width, int height) {
    if (!handle) return CASC_ERR_INVALID_ARG;
    @autoreleasepool {
        casc_ui_mac_t* st = (casc_ui_mac_t*)handle;
        if (st->window) {
            NSRect f = st->window.frame;
            NSRect content = NSMakeRect(0, 0, width, height);
            NSRect framed = [st->window frameRectForContentRect:content];
            f.size = framed.size;
            [st->window setFrame:f display:YES];
        } else if (st->webview) {
            NSRect fr = st->webview.frame;
            fr.size = NSMakeSize(width, height);
            st->webview.frame = fr;
        }
        return CASC_OK;
    }
}

#endif /* __APPLE__ */
