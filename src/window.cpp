#include "window.hpp"

#include "data_url.hpp"
#include "globals.hpp"
#include "key.hpp"
#include "root_widget.hpp"
#include "timeout.hpp"

#include "include/cef_client.h"

namespace browservice {

class Window::Client :
    public CefClient,
    public CefLifeSpanHandler,
    public CefLoadHandler,
    public CefDisplayHandler,
    public CefRequestHandler,
    public CefFindHandler,
    public CefKeyboardHandler
{
public:
    Client(shared_ptr<Window> window) {
        REQUIRE(window);
        window_ = window;

        REQUIRE(window->rootWidget_);
        renderHandler_ =
            window->rootWidget_->browserArea()->createCefRenderHandler();

        lastFindID_ = -1;
        certificateErrorPageSignKey_ = generateDataURLSignKey();
    }

    // CefClient:
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    virtual CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return renderHandler_;
    }
    virtual CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }
    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }
    virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return this;
    }
    virtual CefRefPtr<CefFindHandler> GetFindHandler() override {
        return this;
    }
    virtual CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
        return this;
    }

    // CefLifeSpanHandler:
    virtual bool OnBeforePopup(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString&,
        const CefString&,
        CefLifeSpanHandler::WindowOpenDisposition,
        bool,
        const CefPopupFeatures&,
        CefWindowInfo& windowInfo,
        CefRefPtr<CefClient>& client,
        CefBrowserSettings& browserSettings,
        CefRefPtr<CefDictionaryValue>&,
        bool*
    ) override {
        REQUIRE_UI_THREAD();
        REQUIRE(window_->state_ == Open || window_->state_ == Closed);
        REQUIRE(browser);
        REQUIRE(window_->browser_);
        REQUIRE(window_->browser_->IsSame(browser));

        if(window_->state_ == Open) {
            INFO_LOG(
                "CEF browser of window ", window_->handle_,
                " is requesting a popup window"
            );

            shared_ptr<bool> allowAcceptCall(new bool);
            bool acceptCalled = false;
            bool popupDenied = true;

            auto accept = [&, allowAcceptCall](
                uint64_t newHandle
            ) -> shared_ptr<Window> {
                REQUIRE(*allowAcceptCall);
                REQUIRE(!acceptCalled);
                acceptCalled = true;

                REQUIRE(newHandle);
                REQUIRE(newHandle != window_->handle_);

                INFO_LOG(
                    "Creating window ", newHandle,
                    " (popup of window ", window_->handle_, ")"
                );

                shared_ptr<Window> newWindow = Window::create(Window::CKey());
                newWindow->init_(window_->eventHandler_, newHandle);

                windowInfo.SetAsWindowless(kNullWindowHandle);
                browserSettings.background_color = (cef_color_t)-1;
                client = new Client(newWindow);

                newWindow->createSuccessful_();

                popupDenied = false;
                return newWindow;
            };

            REQUIRE(window_->eventHandler_);
            *allowAcceptCall = true;
            window_->eventHandler_->onWindowCreatePopupRequest(
                window_->handle_, accept
            );
            *allowAcceptCall = false;

            return popupDenied;
        } else {
            return true;
        }
    }

    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        REQUIRE_UI_THREAD();
        REQUIRE(window_->state_ == Open || window_->state_ == Closed);
        REQUIRE(browser);
        REQUIRE(!window_->browser_);

        INFO_LOG("CEF browser for window ", window_->handle_, " created");

        window_->browser_ = browser;
        window_->rootWidget_->browserArea()->setBrowser(browser);

        window_->updateSecurityStatus_();

        if(window_->state_ == Closed) {
            // Browser close deferred from close().
            postTask([browser] {
                browser->GetHost()->CloseBrowser(true);
            });
        }
    }

#define BROWSER_EVENT_HANDLER_CHECKS() \
    do { \
        REQUIRE_UI_THREAD(); \
        REQUIRE(window_->state_ == Open || window_->state_ == Closed); \
        REQUIRE(browser); \
        REQUIRE(window_->browser_); \
        REQUIRE(window_->browser_->IsSame(browser)); \
    } while(false)

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ == Open) {
            // The window closed on its own (not triggered by close()).
            INFO_LOG(
                "Closing window ", window_->handle_, " because ",
                "the CEF browser is closing"
            );
            REQUIRE(window_->eventHandler_);
            window_->state_ = Closed;
            window_->watchdogTimeout_->clear(false);
            window_->eventHandler_->onWindowClose(window_->handle_);
        }

        REQUIRE(window_->state_ == Closed);
        REQUIRE(window_->eventHandler_);

        INFO_LOG(
            "Cleanup of CEF browser for window ", window_->handle_, " complete"
        );

        window_->state_ = CleanupComplete;
        window_->browser_ = nullptr;
        window_->rootWidget_->browserArea()->setBrowser(nullptr);
        window_->eventHandler_->onWindowCleanupComplete(window_->handle_);
        window_->eventHandler_.reset();
    }

    // CefLoadHandler:
    virtual void OnLoadStart(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        TransitionType transitionType
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ == Open && frame->IsMain()) {
            if(readSignedDataURL(frame->GetURL(), certificateErrorPageSignKey_)) {
                window_->rootWidget_->browserArea()->showError(
                    "Loading URL failed due to a certificate error"
                );
            } else {
                window_->rootWidget_->browserArea()->clearError();
            }

            // Make sure that the loaded page gets the correct idea about the
            // focus and mouse over status
            window_->rootWidget_->browserArea()->refreshStatusEvents();
        }
    }

    virtual void OnLoadingStateChange(
        CefRefPtr<CefBrowser> browser,
        bool isLoading,
        bool canGoBack,
        bool canGoForward
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ == Open) {
            window_->rootWidget_->controlBar()->setLoading(isLoading);
            window_->updateSecurityStatus_();
        }
    }

    virtual void OnLoadError(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        ErrorCode errorCode,
        const CefString& errorText,
        const CefString& failedURL
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ != Open || !frame->IsMain()) {
            return;
        }

        if(
            errorCode == ERR_ABORTED &&
            lastCertificateErrorURL_ &&
            *lastCertificateErrorURL_ == string(failedURL)
        ) {
            frame->LoadURL(
                createSignedDataURL(failedURL, certificateErrorPageSignKey_)
            );
        } else if(errorCode != ERR_ABORTED) {
            string msg = "Loading URL failed due to error: " + string(errorText);
            window_->rootWidget_->browserArea()->showError(msg);
            window_->rootWidget_->controlBar()->setAddress(failedURL);
        }
    }

    // CefDisplayHandler:
    virtual void OnAddressChange(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& url
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ != Open) {
            return;
        }

        optional<string> errorURL =
            readSignedDataURL(url, certificateErrorPageSignKey_);
        if(errorURL) {
            window_->rootWidget_->controlBar()->setAddress(*errorURL);
        } else {
            window_->rootWidget_->controlBar()->setAddress(url);
        }
        window_->updateSecurityStatus_();
    }

    virtual bool OnCursorChange(
        CefRefPtr<CefBrowser> browser,
        CefCursorHandle cursorHandle,
        cef_cursor_type_t type,
        const CefCursorInfo& customCursorInfo
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ != Open) {
            return true;
        }

        int cursor = NormalCursor;
        if(type == CT_HAND) cursor = HandCursor;
        if(type == CT_IBEAM) cursor = TextCursor;

        window_->rootWidget_->browserArea()->setCursor(cursor);
        return true;
    }

    // CefRequestHandler:
    virtual CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool isNavigation,
        bool isDownload,
        const CefString& requestInitiator,
        bool& disableDefaultHandling
    ) override {
        CEF_REQUIRE_IO_THREAD();

        postTask(window_, &Window::updateSecurityStatus_);
        return nullptr;
    }

    virtual bool OnCertificateError(
        CefRefPtr<CefBrowser> browser,
        cef_errorcode_t certError,
        const CefString& requestURL,
        CefRefPtr<CefSSLInfo> sslInfo,
        CefRefPtr<CefRequestCallback> callback
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        lastCertificateErrorURL_ = string(requestURL);
        return false;
    }

    // CefFindHandler:
    virtual void OnFindResult(
        CefRefPtr<CefBrowser> browser,
        int identifier,
        int count,
        const CefRect& selectionRect,
        int activeMatchOrdinal,
        bool finalUpdate
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(window_->state_ == Open && identifier >= lastFindID_) {
            if(count > 0 || finalUpdate) {
                window_->rootWidget_->controlBar()->setFindResult(count > 0);
            }
            lastFindID_ = identifier;
        }
    }

    // CefKeyboardHandler:
    virtual bool OnPreKeyEvent(
        CefRefPtr<CefBrowser> browser,
        const CefKeyEvent& event,
        CefEventHandle osEvent,
        bool* isKeyboardShortcut
    ) override {
        BROWSER_EVENT_HANDLER_CHECKS();

        if(
            window_->state_ == Open &&
            event.windows_key_code == -keys::Backspace &&
            !event.focus_on_editable_field
        ) {
            window_->navigate(
                (event.modifiers & EVENTFLAG_SHIFT_DOWN) ? 1 : -1
            );
            return true;
        }
        return false;
    }

private:
    shared_ptr<Window> window_;
    CefRefPtr<CefRenderHandler> renderHandler_;

    int lastFindID_;
    optional<string> lastCertificateErrorURL_;
    string certificateErrorPageSignKey_;

    IMPLEMENT_REFCOUNTING(Client);
};

shared_ptr<Window> Window::tryCreate(
    shared_ptr<WindowEventHandler> eventHandler,
    uint64_t handle
) {
    REQUIRE_UI_THREAD();
    REQUIRE(eventHandler);
    REQUIRE(handle);

    INFO_LOG("Creating window ", handle);

    shared_ptr<Window> window = Window::create(CKey());
    window->init_(eventHandler, handle);

    CefRefPtr<CefClient> client = new Client(window);

    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless(kNullWindowHandle);

    CefBrowserSettings browserSettings;
    browserSettings.background_color = (cef_color_t)-1;

    if(!CefBrowserHost::CreateBrowser(
        windowInfo,
        client,
        globals->config->startPage,
        browserSettings,
        nullptr,
        nullptr
    )) {
        WARNING_LOG(
            "Opening CEF browser for window ", handle, " failed, ",
            "aborting window creation"
        );
        window->createFailed_();
        return {};
    }

    window->createSuccessful_();

    return window;
}

Window::Window(CKey, CKey) {}

Window::~Window() {
    REQUIRE(state_ == CleanupComplete);
}

void Window::close() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    INFO_LOG("Closing window ", handle_);
    state_ = Closed;
    watchdogTimeout_->clear(false);

    // If the browser has been created, we start closing it; otherwise, we defer
    // closing it to Client::OnAfterCreated.
    if(browser_) {
        CefRefPtr<CefBrowser> browser = browser_;
        postTask([browser] {
            browser->GetHost()->CloseBrowser(true);
        });
    }
}

void Window::resize(int width, int height) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    width = max(min(width, 4096), 64);
    height = max(min(height, 4096), 64);

    if(rootViewport_.width() != width || rootViewport_.height() != height) {
        rootViewport_ = ImageSlice::createImage(width, height);
        rootWidget_->setViewport(rootViewport_);
    }
}

ImageSlice Window::fetchViewImage() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    imageChanged_ = false;
    return rootViewport_;
}

void Window::navigate(int direction) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);
    REQUIRE(direction >= -1 && direction <= 1);

    if(browser_) {
        if(direction == -1) {
            browser_->GoBack();
        }
        if(direction == 0) {
            browser_->Reload();
        }
        if(direction == 1) {
            browser_->GoForward();
        }
    }
}

void Window::sendMouseDownEvent(int x, int y, int button) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    if(button >= 0 && button <= 2) {
        clampMouseCoords_(x, y);
        rootWidget_->sendMouseDownEvent(x, y, button);
    }
}

void Window::sendMouseUpEvent(int x, int y, int button) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    if(button >= 0 && button <= 2) {
        clampMouseCoords_(x, y);
        rootWidget_->sendMouseUpEvent(x, y, button);
    }
}

void Window::sendMouseMoveEvent(int x, int y) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    clampMouseCoords_(x, y);
    rootWidget_->sendMouseMoveEvent(x, y);
}

void Window::sendMouseDoubleClickEvent(int x, int y, int button) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    if(button == 0) {
        clampMouseCoords_(x, y);
        rootWidget_->sendMouseDoubleClickEvent(x, y);
    }
}

void Window::sendMouseWheelEvent(int x, int y, int dx, int dy) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    clampMouseCoords_(x, y);
    int delta = max(-180, min(180, -dy));
    rootWidget_->sendMouseWheelEvent(x, y, delta);
}

void Window::sendMouseLeaveEvent(int x, int y) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    clampMouseCoords_(x, y);
    rootWidget_->sendMouseLeaveEvent(x, y);
}

void Window::sendKeyDownEvent(int key) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    if(isValidKey(key)) {
        rootWidget_->sendKeyDownEvent(key);
    }
}

void Window::sendKeyUpEvent(int key) {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    if(isValidKey(key)) {
        rootWidget_->sendKeyUpEvent(key);
    }
}

void Window::sendLoseFocusEvent() {
    REQUIRE_UI_THREAD();
    REQUIRE(state_ == Open);

    rootWidget_->sendLoseFocusEvent();
}

void Window::onWidgetViewDirty() {
    REQUIRE_UI_THREAD();

    if(state_ != Open) {
        return;
    }

    shared_ptr<Window> self = shared_from_this();
    postTask([self]() {
        if(self->state_ == Open) {
            self->rootWidget_->render();
            self->signalImageChanged_();
        }
    });
}

void Window::onWidgetCursorChanged() {
    REQUIRE_UI_THREAD();

    if(state_ != Open) {
        return;
    }

    shared_ptr<Window> self = shared_from_this();
    postTask([self]() {
        if(self->state_ == Open) {
            int cursor = self->rootWidget_->cursor();
            REQUIRE(cursor >= 0 && cursor < CursorTypeCount);

            REQUIRE(self->eventHandler_);
            self->eventHandler_->onWindowCursorChanged(self->handle_, cursor);
        }
    });
}

void Window::onGlobalHotkeyPressed(GlobalHotkey key) {
    REQUIRE_UI_THREAD();

    if(state_ != Open) {
        return;
    }

    shared_ptr<Window> self = shared_from_this();
    postTask([self, key]() {
        if(self->state_ == Open) {
            if(key == GlobalHotkey::Address) {
                self->rootWidget_->controlBar()->activateAddress();
            }
            if(key == GlobalHotkey::Find) {
                self->rootWidget_->controlBar()->openFindBar();
            }
            if(key == GlobalHotkey::FindNext) {
                self->rootWidget_->controlBar()->findNext();
            }
            if(key == GlobalHotkey::Refresh) {
                self->navigate(0);
            }
        }
    });
}

void Window::onAddressSubmitted(string url) {
    REQUIRE_UI_THREAD();

    if(state_ != Open || !browser_ || url.empty()) {
        return;
    }

    CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
    if(frame) {
        frame->LoadURL(url);
        rootWidget_->browserArea()->takeFocus();
    }
}

void Window::onFind(string text, bool forward, bool findNext) {
    REQUIRE_UI_THREAD();

    if(state_ == Open && browser_) {
        browser_->GetHost()->Find(0, text, forward, false, findNext);
    }
}

void Window::onStopFind(bool clearSelection) {
    REQUIRE_UI_THREAD();

    if(state_ == Open && browser_) {
        browser_->GetHost()->StopFinding(clearSelection);
    }
}

void Window::onClipboardButtonPressed() {
    REQUIRE_UI_THREAD();

    if(state_ == Open) {
        REQUIRE(eventHandler_);
        eventHandler_->onWindowClipboardButtonPressed(handle_);
    }
}

void Window::onBrowserAreaViewDirty() {
    REQUIRE_UI_THREAD();

    if(state_ == Open) {
        signalImageChanged_();
    }
}

void Window::init_(shared_ptr<WindowEventHandler> eventHandler, uint64_t handle) {
    REQUIRE_UI_THREAD();
    REQUIRE(eventHandler);
    REQUIRE(handle);

    handle_ = handle;
    state_ = Open;
    eventHandler_ = eventHandler;

    imageChanged_ = false;

    shared_ptr<Window> self = shared_from_this();

    rootViewport_ = ImageSlice::createImage(800, 600);
    rootWidget_ = RootWidget::create(self, self, self, true);
    rootWidget_->setViewport(rootViewport_);

    watchdogTimeout_ = Timeout::create(1000);
}

void Window::createSuccessful_() {
    REQUIRE_UI_THREAD();

    shared_ptr<Window> self = shared_from_this();

    postTask(self, &Window::watchdog_);

    postTask([self]() {
        if(self->state_ != Open) {
            return;
        }

        REQUIRE(self->eventHandler_);

        optional<pair<vector<string>, size_t>> qualitySelector =
            self->eventHandler_->onWindowQualitySelectorQuery(self->handle_);
        if(qualitySelector) {
            self->rootWidget_->controlBar()->enableQualitySelector(
                move(qualitySelector->first), qualitySelector->second
            );
        }

        if(self->eventHandler_->onWindowNeedsClipboardButtonQuery(
            self->handle_
        )) {
            self->rootWidget_->controlBar()->enableClipboardButton();
        }
    });
}

void Window::createFailed_() {
    REQUIRE_UI_THREAD();

    state_ = CleanupComplete;
    eventHandler_.reset();
}

// Called every 1s for an Open window for various checks.
void Window::watchdog_() {
    REQUIRE_UI_THREAD();

    if(state_ != Open) {
        return;
    }

    // Make sure that security status is not incorrect for extended periods of
    // time just in case our event handlers do not catch all the changes.
    updateSecurityStatus_();

    if(!watchdogTimeout_->isActive()) {
        weak_ptr<Window> selfWeak = shared_from_this();
        watchdogTimeout_->set([selfWeak]() {
            if(shared_ptr<Window> self = selfWeak.lock()) {
                self->watchdog_();
            }
        });
    }
}

void Window::updateSecurityStatus_() {
    REQUIRE_UI_THREAD();

    if(state_ != Open) {
        return;
    }

    SecurityStatus securityStatus = SecurityStatus::Insecure;
    if(browser_) {
        CefRefPtr<CefNavigationEntry> nav = browser_->GetHost()->GetVisibleNavigationEntry();
        if(nav) {
            CefRefPtr<CefSSLStatus> sslStatus = nav->GetSSLStatus();
            if(
                sslStatus &&
                sslStatus->IsSecureConnection() &&
                !(sslStatus->GetCertStatus() & ~(
                    // non-error statuses
                    CERT_STATUS_IS_EV |
                    CERT_STATUS_REV_CHECKING_ENABLED |
                    CERT_STATUS_SHA1_SIGNATURE_PRESENT |
                    CERT_STATUS_CT_COMPLIANCE_FAILED
                ))
            ) {
                if(sslStatus->GetContentStatus() == SSL_CONTENT_NORMAL_CONTENT) {
                    securityStatus = SecurityStatus::Secure;
                } else {
                    securityStatus = SecurityStatus::Warning;
                }
            }
        }
    }

    rootWidget_->controlBar()->setSecurityStatus(securityStatus);
}

void Window::clampMouseCoords_(int& x, int& y) {
    x = max(x, -1000);
    y = max(y, -1000);
    x = min(x, rootViewport_.width() + 1000);
    y = min(y, rootViewport_.height() + 1000);
}

void Window::signalImageChanged_() {
    REQUIRE_UI_THREAD();

    if(state_ == Open && !imageChanged_) {
        imageChanged_ = true;

        REQUIRE(eventHandler_);
        eventHandler_->onWindowViewImageChanged(handle_);
    }
}

}
