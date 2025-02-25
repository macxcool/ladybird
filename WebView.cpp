/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include "WebView.h"
#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringBuilder.h>
#include <AK/Types.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/IODevice.h>
#include <LibCore/MemoryStream.h>
#include <LibCore/Stream.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGemini/GeminiRequest.h>
#include <LibGemini/GeminiResponse.h>
#include <LibGemini/Job.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageDecoder.h>
#include <LibGfx/PNGWriter.h>
#include <LibGfx/Rect.h>
#include <LibHTTP/HttpRequest.h>
#include <LibHTTP/HttpResponse.h>
#include <LibHTTP/HttpsJob.h>
#include <LibHTTP/Job.h>
#include <LibMain/Main.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/ImageDecoding.h>
#include <LibWeb/Layout/InitialContainingBlock.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/WebSockets/WebSocket.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Message.h>
#include <LibWebSocket/WebSocket.h>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <stdlib.h>

String s_serenity_resource_root = [] {
    auto const* source_dir = getenv("SERENITY_SOURCE_DIR");
    if (source_dir) {
        return String::formatted("{}/Base", source_dir);
    }
    auto* home = getenv("XDG_CONFIG_HOME") ?: getenv("HOME");
    VERIFY(home);
    return String::formatted("{}/.lagom", home);
}();

class HeadlessBrowserPageClient final : public Web::PageClient {
public:
    static NonnullOwnPtr<HeadlessBrowserPageClient> create(WebView& view)
    {
        return adopt_own(*new HeadlessBrowserPageClient(view));
    }

    Web::Page& page() { return *m_page; }
    Web::Page const& page() const { return *m_page; }

    Web::Layout::InitialContainingBlock* layout_root()
    {
        auto* document = page().top_level_browsing_context().active_document();
        if (!document)
            return nullptr;
        return document->layout_node();
    }

    void load(AK::URL const& url)
    {
        page().load(url);
    }

    void paint(Gfx::IntRect const& content_rect, Gfx::Bitmap& target)
    {
        Gfx::Painter painter(target);

        if (auto* document = page().top_level_browsing_context().active_document())
            document->update_layout();

        painter.fill_rect({ {}, content_rect.size() }, palette().base());

        auto* layout_root = this->layout_root();
        if (!layout_root) {
            return;
        }

        Web::PaintContext context(painter, palette(), content_rect.top_left());
        context.set_should_show_line_box_borders(false);
        context.set_viewport_rect(content_rect);
        context.set_has_focus(true);
        layout_root->paint_all_phases(context);
    }

    void setup_palette(Core::AnonymousBuffer theme_buffer)
    {
        m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(theme_buffer);
    }

    void set_viewport_rect(Gfx::IntRect rect)
    {
        m_viewport_rect = rect;
        page().top_level_browsing_context().set_viewport_rect(rect);
    }

    // ^Web::PageClient
    virtual Gfx::Palette palette() const override
    {
        return Gfx::Palette(*m_palette_impl);
    }

    virtual Gfx::IntRect screen_rect() const override
    {
        // FIXME: Return the actual screen rect.
        return m_viewport_rect;
    }

    Gfx::IntRect viewport_rect() const
    {
        return m_viewport_rect;
    }

    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override
    {
        return m_preferred_color_scheme;
    }

    virtual void page_did_change_title(String const& title) override
    {
        emit m_view.title_changed(title.characters());
    }

    virtual void page_did_set_document_in_top_level_browsing_context(Web::DOM::Document*) override
    {
    }

    virtual void page_did_start_loading(AK::URL const& url) override
    {
        emit m_view.loadStarted(url.to_string().characters());
    }

    virtual void page_did_finish_loading(AK::URL const&) override
    {
    }

    virtual void page_did_change_selection() override
    {
    }

    virtual void page_did_request_cursor_change(Gfx::StandardCursor) override
    {
    }

    virtual void page_did_request_context_menu(Gfx::IntPoint const&) override
    {
    }

    virtual void page_did_request_link_context_menu(Gfx::IntPoint const&, AK::URL const&, String const&, unsigned) override
    {
    }

    virtual void page_did_request_image_context_menu(Gfx::IntPoint const&, AK::URL const&, String const&, unsigned, Gfx::Bitmap const*) override
    {
    }

    virtual void page_did_click_link(AK::URL const&, String const&, unsigned) override
    {
    }

    virtual void page_did_middle_click_link(AK::URL const&, String const&, unsigned) override
    {
    }

    virtual void page_did_enter_tooltip_area(Gfx::IntPoint const&, String const&) override
    {
    }

    virtual void page_did_leave_tooltip_area() override
    {
    }

    virtual void page_did_hover_link(AK::URL const& url) override
    {
        emit m_view.linkHovered(url.to_string().characters());
    }

    virtual void page_did_unhover_link() override
    {
        emit m_view.linkUnhovered();
    }

    virtual void page_did_invalidate(Gfx::IntRect const&) override
    {
        m_view.viewport()->update();
    }

    virtual void page_did_change_favicon(Gfx::Bitmap const&) override
    {
    }

    virtual void page_did_layout() override
    {
        auto* layout_root = this->layout_root();
        VERIFY(layout_root);
        Gfx::IntSize content_size;
        if (layout_root->paint_box()->has_overflow())
            content_size = enclosing_int_rect(layout_root->paint_box()->scrollable_overflow_rect().value()).size();
        else
            content_size = enclosing_int_rect(layout_root->paint_box()->absolute_rect()).size();

        m_view.verticalScrollBar()->setMaximum(content_size.height() - m_viewport_rect.height());
        m_view.horizontalScrollBar()->setMaximum(content_size.width() - m_viewport_rect.width());
    }

    virtual void page_did_request_scroll_into_view(Gfx::IntRect const&) override
    {
    }

    virtual void page_did_request_alert(String const&) override
    {
    }

    virtual bool page_did_request_confirm(String const&) override
    {
        return false;
    }

    virtual String page_did_request_prompt(String const&, String const&) override
    {
        return String::empty();
    }

    virtual String page_did_request_cookie(AK::URL const&, Web::Cookie::Source) override
    {
        return String::empty();
    }

    virtual void page_did_set_cookie(AK::URL const&, Web::Cookie::ParsedCookie const&, Web::Cookie::Source) override
    {
    }

    void request_file(NonnullRefPtr<Web::FileRequest>& request) override
    {
        auto const file = Core::System::open(request->path(), O_RDONLY);
        request->on_file_request_finish(file);
    }

private:
    HeadlessBrowserPageClient(WebView& view)
        : m_view(view)
        , m_page(make<Web::Page>(*this))
    {
    }

    WebView& m_view;
    NonnullOwnPtr<Web::Page> m_page;

    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Gfx::IntRect m_viewport_rect { 0, 0, 800, 600 };
    Web::CSS::PreferredColorScheme m_preferred_color_scheme { Web::CSS::PreferredColorScheme::Auto };
};

WebView::WebView()
{
    setMouseTracking(true);

    m_page_client = HeadlessBrowserPageClient::create(*this);

    m_page_client->setup_palette(Gfx::load_system_theme(String::formatted("{}/res/themes/Default.ini", s_serenity_resource_root)));

    // FIXME: Allow passing these values as arguments
    m_page_client->set_viewport_rect({ 0, 0, 800, 600 });
}

WebView::~WebView()
{
}

void WebView::load(String const& url)
{
    m_page_client->load(AK::URL(url));
}

unsigned get_button_from_qt_event(QMouseEvent const& event)
{
    if (event.button() == Qt::MouseButton::LeftButton)
        return 1;
    if (event.button() == Qt::MouseButton::RightButton)
        return 2;
    if (event.button() == Qt::MouseButton::MiddleButton)
        return 4;
    return 0;
}

unsigned get_buttons_from_qt_event(QMouseEvent const& event)
{
    unsigned buttons = 0;
    if (event.buttons() & Qt::MouseButton::LeftButton)
        buttons |= 1;
    if (event.buttons() & Qt::MouseButton::RightButton)
        buttons |= 2;
    if (event.buttons() & Qt::MouseButton::MiddleButton)
        buttons |= 4;
    return buttons;
}

unsigned get_modifiers_from_qt_event(QMouseEvent const& event)
{
    unsigned modifiers = 0;
    if (event.modifiers() & Qt::Modifier::ALT)
        modifiers |= 1;
    if (event.modifiers() & Qt::Modifier::CTRL)
        modifiers |= 2;
    if (event.modifiers() & Qt::Modifier::SHIFT)
        modifiers |= 4;
    return modifiers;
}

void WebView::mouseMoveEvent(QMouseEvent* event)
{
    Gfx::IntPoint position(event->x(), event->y());
    auto buttons = get_buttons_from_qt_event(*event);
    auto modifiers = get_modifiers_from_qt_event(*event);
    m_page_client->page().handle_mousemove(to_content(position), buttons, modifiers);
}

void WebView::mousePressEvent(QMouseEvent* event)
{
    Gfx::IntPoint position(event->x(), event->y());
    auto button = get_button_from_qt_event(*event);
    auto modifiers = get_modifiers_from_qt_event(*event);
    m_page_client->page().handle_mousedown(to_content(position), button, modifiers);
}

void WebView::mouseReleaseEvent(QMouseEvent* event)
{
    Gfx::IntPoint position(event->x(), event->y());
    auto button = get_button_from_qt_event(*event);
    auto modifiers = get_modifiers_from_qt_event(*event);
    m_page_client->page().handle_mouseup(to_content(position), button, modifiers);
}

Gfx::IntPoint WebView::to_content(Gfx::IntPoint viewport_position) const
{
    return viewport_position.translated(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

void WebView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.setClipRect(event->rect());

    auto output_rect = m_page_client->viewport_rect();
    output_rect.set_x(horizontalScrollBar()->value());
    output_rect.set_y(verticalScrollBar()->value());
    auto output_bitmap = MUST(Gfx::Bitmap::try_create(Gfx::BitmapFormat::BGRx8888, output_rect.size()));

    m_page_client->paint(output_rect, output_bitmap);

    QImage q_image(output_bitmap->scanline_u8(0), output_bitmap->width(), output_bitmap->height(), QImage::Format_RGB32);
    painter.drawImage(QPoint(0, 0), q_image);
}

void WebView::resizeEvent(QResizeEvent* event)
{
    Gfx::IntRect rect(horizontalScrollBar()->value(), verticalScrollBar()->value(), event->size().width(), event->size().height());
    m_page_client->set_viewport_rect(rect);
}

class HeadlessImageDecoderClient : public Web::ImageDecoding::Decoder {
public:
    static NonnullRefPtr<HeadlessImageDecoderClient> create()
    {
        return adopt_ref(*new HeadlessImageDecoderClient());
    }

    virtual ~HeadlessImageDecoderClient() override = default;

    virtual Optional<Web::ImageDecoding::DecodedImage> decode_image(ReadonlyBytes data) override
    {
        auto decoder = Gfx::ImageDecoder::try_create(data);

        if (!decoder)
            return Web::ImageDecoding::DecodedImage { false, 0, Vector<Web::ImageDecoding::Frame> {} };

        if (!decoder->frame_count())
            return Web::ImageDecoding::DecodedImage { false, 0, Vector<Web::ImageDecoding::Frame> {} };

        Vector<Web::ImageDecoding::Frame> frames;
        for (size_t i = 0; i < decoder->frame_count(); ++i) {
            auto frame_or_error = decoder->frame(i);
            if (frame_or_error.is_error()) {
                frames.append({ {}, 0 });
            } else {
                auto frame = frame_or_error.release_value();
                frames.append({ move(frame.image), static_cast<size_t>(frame.duration) });
            }
        }

        return Web::ImageDecoding::DecodedImage {
            decoder->is_animated(),
            static_cast<u32>(decoder->loop_count()),
            frames,
        };
    }

private:
    explicit HeadlessImageDecoderClient() = default;
};

static HashTable<RefPtr<Web::ResourceLoaderConnectorRequest>> s_all_requests;

class HeadlessRequestServer : public Web::ResourceLoaderConnector {
public:
    class HTTPHeadlessRequest
        : public Web::ResourceLoaderConnectorRequest
        , public Weakable<HTTPHeadlessRequest> {
    public:
        static ErrorOr<NonnullRefPtr<HTTPHeadlessRequest>> create(String const& method, AK::URL const& url, HashMap<String, String> const& request_headers, ReadonlyBytes request_body, Core::ProxyData const&)
        {
            auto stream_backing_buffer = TRY(ByteBuffer::create_uninitialized(1 * MiB));
            auto underlying_socket = TRY(Core::Stream::TCPSocket::connect(url.host(), url.port().value_or(80)));
            TRY(underlying_socket->set_blocking(false));
            auto socket = TRY(Core::Stream::BufferedSocket<Core::Stream::TCPSocket>::create(move(underlying_socket)));

            HTTP::HttpRequest request;
            if (method.equals_ignoring_case("head"sv))
                request.set_method(HTTP::HttpRequest::HEAD);
            else if (method.equals_ignoring_case("get"sv))
                request.set_method(HTTP::HttpRequest::GET);
            else if (method.equals_ignoring_case("post"sv))
                request.set_method(HTTP::HttpRequest::POST);
            else
                request.set_method(HTTP::HttpRequest::Invalid);
            request.set_url(move(url));
            request.set_headers(request_headers);
            request.set_body(TRY(ByteBuffer::copy(request_body)));

            return adopt_ref(*new HTTPHeadlessRequest(move(request), move(socket), move(stream_backing_buffer)));
        }

        virtual ~HTTPHeadlessRequest() override
        {
        }

        virtual void set_should_buffer_all_input(bool) override
        {
        }

        virtual bool stop() override
        {
            return false;
        }

        virtual void stream_into(Core::Stream::Stream&) override
        {
        }

    private:
        HTTPHeadlessRequest(HTTP::HttpRequest&& request, NonnullOwnPtr<Core::Stream::BufferedSocketBase> socket, ByteBuffer&& stream_backing_buffer)
            : m_stream_backing_buffer(move(stream_backing_buffer))
            , m_output_stream(Core::Stream::MemoryStream::construct(m_stream_backing_buffer.bytes()).release_value_but_fixme_should_propagate_errors())
            , m_socket(move(socket))
            , m_job(HTTP::Job::construct(move(request), *m_output_stream))
        {
            m_job->on_headers_received = [weak_this = make_weak_ptr()](auto& response_headers, auto response_code) mutable {
                if (auto strong_this = weak_this.strong_ref()) {
                    strong_this->m_response_code = response_code;
                    for (auto& header : response_headers) {
                        strong_this->m_response_headers.set(header.key, header.value);
                    }
                }
            };
            m_job->on_finish = [weak_this = make_weak_ptr()](bool success) mutable {
                Core::deferred_invoke([weak_this, success]() mutable {
                    if (auto strong_this = weak_this.strong_ref()) {
                        ReadonlyBytes response_bytes { strong_this->m_output_stream->bytes().data(), strong_this->m_output_stream->offset() };
                        auto response_buffer = ByteBuffer::copy(response_bytes).release_value_but_fixme_should_propagate_errors();
                        strong_this->on_buffered_request_finish(success, strong_this->m_output_stream->offset(), strong_this->m_response_headers, strong_this->m_response_code, response_buffer);
                    } });
            };
            m_job->start(*m_socket);
        }

        Optional<u32> m_response_code;
        ByteBuffer m_stream_backing_buffer;
        NonnullOwnPtr<Core::Stream::MemoryStream> m_output_stream;
        NonnullOwnPtr<Core::Stream::BufferedSocketBase> m_socket;
        NonnullRefPtr<HTTP::Job> m_job;
        HashMap<String, String, CaseInsensitiveStringTraits> m_response_headers;
    };

    class HTTPSHeadlessRequest
        : public Web::ResourceLoaderConnectorRequest
        , public Weakable<HTTPSHeadlessRequest> {
    public:
        static ErrorOr<NonnullRefPtr<HTTPSHeadlessRequest>> create(String const& method, AK::URL const& url, HashMap<String, String> const& request_headers, ReadonlyBytes request_body, Core::ProxyData const&)
        {
            auto stream_backing_buffer = TRY(ByteBuffer::create_uninitialized(1 * MiB));
            auto underlying_socket = TRY(TLS::TLSv12::connect(url.host(), url.port().value_or(443)));
            TRY(underlying_socket->set_blocking(false));
            auto socket = TRY(Core::Stream::BufferedSocket<TLS::TLSv12>::create(move(underlying_socket)));

            HTTP::HttpRequest request;
            if (method.equals_ignoring_case("head"sv))
                request.set_method(HTTP::HttpRequest::HEAD);
            else if (method.equals_ignoring_case("get"sv))
                request.set_method(HTTP::HttpRequest::GET);
            else if (method.equals_ignoring_case("post"sv))
                request.set_method(HTTP::HttpRequest::POST);
            else
                request.set_method(HTTP::HttpRequest::Invalid);
            request.set_url(move(url));
            request.set_headers(request_headers);
            request.set_body(TRY(ByteBuffer::copy(request_body)));

            return adopt_ref(*new HTTPSHeadlessRequest(move(request), move(socket), move(stream_backing_buffer)));
        }

        virtual ~HTTPSHeadlessRequest() override
        {
        }

        virtual void set_should_buffer_all_input(bool) override
        {
        }

        virtual bool stop() override
        {
            return false;
        }

        virtual void stream_into(Core::Stream::Stream&) override
        {
        }

    private:
        HTTPSHeadlessRequest(HTTP::HttpRequest&& request, NonnullOwnPtr<Core::Stream::BufferedSocketBase> socket, ByteBuffer&& stream_backing_buffer)
            : m_stream_backing_buffer(move(stream_backing_buffer))
            , m_output_stream(Core::Stream::MemoryStream::construct(m_stream_backing_buffer.bytes()).release_value_but_fixme_should_propagate_errors())
            , m_socket(move(socket))
            , m_job(HTTP::HttpsJob::construct(move(request), *m_output_stream))
        {
            m_job->on_headers_received = [weak_this = make_weak_ptr()](auto& response_headers, auto response_code) mutable {
                if (auto strong_this = weak_this.strong_ref()) {
                    strong_this->m_response_code = response_code;
                    for (auto& header : response_headers) {
                        strong_this->m_response_headers.set(header.key, header.value);
                    }
                }
            };
            m_job->on_finish = [weak_this = make_weak_ptr()](bool success) mutable {
                Core::deferred_invoke([weak_this, success]() mutable {
                    if (auto strong_this = weak_this.strong_ref()) {
                        ReadonlyBytes response_bytes { strong_this->m_output_stream->bytes().data(), strong_this->m_output_stream->offset() };
                        auto response_buffer = ByteBuffer::copy(response_bytes).release_value_but_fixme_should_propagate_errors();
                        strong_this->on_buffered_request_finish(success, strong_this->m_output_stream->offset(), strong_this->m_response_headers, strong_this->m_response_code, response_buffer);
                    } });
            };
            m_job->start(*m_socket);
        }

        Optional<u32> m_response_code;
        ByteBuffer m_stream_backing_buffer;
        NonnullOwnPtr<Core::Stream::MemoryStream> m_output_stream;
        NonnullOwnPtr<Core::Stream::BufferedSocketBase> m_socket;
        NonnullRefPtr<HTTP::HttpsJob> m_job;
        HashMap<String, String, CaseInsensitiveStringTraits> m_response_headers;
    };

    class GeminiHeadlessRequest
        : public Web::ResourceLoaderConnectorRequest
        , public Weakable<GeminiHeadlessRequest> {
    public:
        static ErrorOr<NonnullRefPtr<GeminiHeadlessRequest>> create(String const&, AK::URL const& url, HashMap<String, String> const&, ReadonlyBytes, Core::ProxyData const&)
        {
            auto stream_backing_buffer = TRY(ByteBuffer::create_uninitialized(1 * MiB));
            auto underlying_socket = TRY(Core::Stream::TCPSocket::connect(url.host(), url.port().value_or(80)));
            TRY(underlying_socket->set_blocking(false));
            auto socket = TRY(Core::Stream::BufferedSocket<Core::Stream::TCPSocket>::create(move(underlying_socket)));

            Gemini::GeminiRequest request;
            request.set_url(url);

            return adopt_ref(*new GeminiHeadlessRequest(move(request), move(socket), move(stream_backing_buffer)));
        }

        virtual ~GeminiHeadlessRequest() override
        {
        }

        virtual void set_should_buffer_all_input(bool) override
        {
        }

        virtual bool stop() override
        {
            return false;
        }

        virtual void stream_into(Core::Stream::Stream&) override
        {
        }

    private:
        GeminiHeadlessRequest(Gemini::GeminiRequest&& request, NonnullOwnPtr<Core::Stream::BufferedSocketBase> socket, ByteBuffer&& stream_backing_buffer)
            : m_stream_backing_buffer(move(stream_backing_buffer))
            , m_output_stream(Core::Stream::MemoryStream::construct(m_stream_backing_buffer.bytes()).release_value_but_fixme_should_propagate_errors())
            , m_socket(move(socket))
            , m_job(Gemini::Job::construct(move(request), *m_output_stream))
        {
            m_job->on_headers_received = [weak_this = make_weak_ptr()](auto& response_headers, auto response_code) mutable {
                if (auto strong_this = weak_this.strong_ref()) {
                    strong_this->m_response_code = response_code;
                    for (auto& header : response_headers) {
                        strong_this->m_response_headers.set(header.key, header.value);
                    }
                }
            };
            m_job->on_finish = [weak_this = make_weak_ptr()](bool success) mutable {
                Core::deferred_invoke([weak_this, success]() mutable {
                    if (auto strong_this = weak_this.strong_ref()) {
                        ReadonlyBytes response_bytes { strong_this->m_output_stream->bytes().data(), strong_this->m_output_stream->offset() };
                        auto response_buffer = ByteBuffer::copy(response_bytes).release_value_but_fixme_should_propagate_errors();
                        strong_this->on_buffered_request_finish(success, strong_this->m_output_stream->offset(), strong_this->m_response_headers, strong_this->m_response_code, response_buffer);
                    } });
            };
            m_job->start(*m_socket);
        }

        Optional<u32> m_response_code;
        ByteBuffer m_stream_backing_buffer;
        NonnullOwnPtr<Core::Stream::MemoryStream> m_output_stream;
        NonnullOwnPtr<Core::Stream::BufferedSocketBase> m_socket;
        NonnullRefPtr<Gemini::Job> m_job;
        HashMap<String, String, CaseInsensitiveStringTraits> m_response_headers;
    };

    static NonnullRefPtr<HeadlessRequestServer> create()
    {
        return adopt_ref(*new HeadlessRequestServer());
    }

    virtual ~HeadlessRequestServer() override { }

    virtual void prefetch_dns(AK::URL const&) override { }
    virtual void preconnect(AK::URL const&) override { }

    virtual RefPtr<Web::ResourceLoaderConnectorRequest> start_request(String const& method, AK::URL const& url, HashMap<String, String> const& request_headers, ReadonlyBytes request_body, Core::ProxyData const& proxy) override
    {
        RefPtr<Web::ResourceLoaderConnectorRequest> request;
        if (url.protocol().equals_ignoring_case("http"sv)) {
            auto request_or_error = HTTPHeadlessRequest::create(method, url, request_headers, request_body, proxy);
            if (request_or_error.is_error())
                return {};
            request = request_or_error.release_value();
        }
        if (url.protocol().equals_ignoring_case("https"sv)) {
            auto request_or_error = HTTPSHeadlessRequest::create(method, url, request_headers, request_body, proxy);
            if (request_or_error.is_error())
                return {};
            request = request_or_error.release_value();
        }
        if (url.protocol().equals_ignoring_case("gemini"sv)) {
            auto request_or_error = GeminiHeadlessRequest::create(method, url, request_headers, request_body, proxy);
            if (request_or_error.is_error())
                return {};
            request = request_or_error.release_value();
        }
        if (request)
            s_all_requests.set(request);
        return request;
    }

private:
    HeadlessRequestServer() { }
};

class HeadlessWebSocketClientManager : public Web::WebSockets::WebSocketClientManager {
public:
    class HeadlessWebSocket
        : public Web::WebSockets::WebSocketClientSocket
        , public Weakable<HeadlessWebSocket> {
    public:
        static NonnullRefPtr<HeadlessWebSocket> create(NonnullRefPtr<WebSocket::WebSocket> underlying_socket)
        {
            return adopt_ref(*new HeadlessWebSocket(move(underlying_socket)));
        }

        virtual ~HeadlessWebSocket() override
        {
        }

        virtual Web::WebSockets::WebSocket::ReadyState ready_state() override
        {
            switch (m_websocket->ready_state()) {
            case WebSocket::ReadyState::Connecting:
                return Web::WebSockets::WebSocket::ReadyState::Connecting;
            case WebSocket::ReadyState::Open:
                return Web::WebSockets::WebSocket::ReadyState::Open;
            case WebSocket::ReadyState::Closing:
                return Web::WebSockets::WebSocket::ReadyState::Closing;
            case WebSocket::ReadyState::Closed:
                return Web::WebSockets::WebSocket::ReadyState::Closed;
            }
            VERIFY_NOT_REACHED();
        }

        virtual void send(ByteBuffer binary_or_text_message, bool is_text) override
        {
            m_websocket->send(WebSocket::Message(binary_or_text_message, is_text));
        }

        virtual void send(StringView message) override
        {
            m_websocket->send(WebSocket::Message(message));
        }

        virtual void close(u16 code, String reason) override
        {
            m_websocket->close(code, reason);
        }

    private:
        HeadlessWebSocket(NonnullRefPtr<WebSocket::WebSocket> underlying_socket)
            : m_websocket(move(underlying_socket))
        {
            m_websocket->on_open = [weak_this = make_weak_ptr()] {
                if (auto strong_this = weak_this.strong_ref())
                    if (strong_this->on_open)
                        strong_this->on_open();
            };
            m_websocket->on_message = [weak_this = make_weak_ptr()](auto message) {
                if (auto strong_this = weak_this.strong_ref()) {
                    if (strong_this->on_message) {
                        strong_this->on_message(Web::WebSockets::WebSocketClientSocket::Message {
                            .data = move(message.data()),
                            .is_text = message.is_text(),
                        });
                    }
                }
            };
            m_websocket->on_error = [weak_this = make_weak_ptr()](auto error) {
                if (auto strong_this = weak_this.strong_ref()) {
                    if (strong_this->on_error) {
                        switch (error) {
                        case WebSocket::WebSocket::Error::CouldNotEstablishConnection:
                            strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::CouldNotEstablishConnection);
                            return;
                        case WebSocket::WebSocket::Error::ConnectionUpgradeFailed:
                            strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::ConnectionUpgradeFailed);
                            return;
                        case WebSocket::WebSocket::Error::ServerClosedSocket:
                            strong_this->on_error(Web::WebSockets::WebSocketClientSocket::Error::ServerClosedSocket);
                            return;
                        }
                        VERIFY_NOT_REACHED();
                    }
                }
            };
            m_websocket->on_close = [weak_this = make_weak_ptr()](u16 code, String reason, bool was_clean) {
                if (auto strong_this = weak_this.strong_ref())
                    if (strong_this->on_close)
                        strong_this->on_close(code, move(reason), was_clean);
            };
        }

        NonnullRefPtr<WebSocket::WebSocket> m_websocket;
    };

    static NonnullRefPtr<HeadlessWebSocketClientManager> create()
    {
        return adopt_ref(*new HeadlessWebSocketClientManager());
    }

    virtual ~HeadlessWebSocketClientManager() override { }

    virtual RefPtr<Web::WebSockets::WebSocketClientSocket> connect(AK::URL const& url, String const& origin) override
    {
        WebSocket::ConnectionInfo connection_info(url);
        connection_info.set_origin(origin);

        auto connection = HeadlessWebSocket::create(WebSocket::WebSocket::create(move(connection_info)));
        return connection;
    }

private:
    HeadlessWebSocketClientManager() { }
};

void initialize_web_engine()
{
    Web::ImageDecoding::Decoder::initialize(HeadlessImageDecoderClient::create());
    Web::ResourceLoader::initialize(HeadlessRequestServer::create());
    Web::WebSockets::WebSocketClientManager::initialize(HeadlessWebSocketClientManager::create());

    Web::FrameLoader::set_default_favicon_path(String::formatted("{}/res/icons/16x16/app-browser.png", s_serenity_resource_root));
    dbgln("Set favoicon path to {}", String::formatted("{}/res/icons/16x16/app-browser.png", s_serenity_resource_root));

    Gfx::FontDatabase::set_default_fonts_lookup_path(String::formatted("{}/res/fonts", s_serenity_resource_root));

    Gfx::FontDatabase::set_default_font_query("Katica 10 400 0");
    Gfx::FontDatabase::set_fixed_width_font_query("Csilla 10 400 0");

    Web::FrameLoader::set_error_page_url(String::formatted("file://{}/res/html/error.html", s_serenity_resource_root));
}
