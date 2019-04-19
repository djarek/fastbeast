#include "http_session.hpp"
#include "log.hpp"

#include "experimental_serializer.hpp"

#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/span_body.hpp>
#include <boost/intrusive/list.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace fastbeast
{

namespace http = boost::beast::http;
namespace beast = boost::beast;
using error_code = boost::system::error_code;
using string_view = beast::string_view;

struct monotonic_pool
{
    void reset()
    {
        position = buf.data();
        size = buf.size();
    }

    std::array<char, 16384> buf;
    void* position = buf.data();
    std::size_t size = buf.size();
};

template<class T>
struct monotonic_allocator
{
    using value_type = T;

    explicit monotonic_allocator(monotonic_pool& pool)
      : pool_{&pool}
    {
    }

    template<class U>
    monotonic_allocator(monotonic_allocator<U> const& other)
      : pool_{other.pool_}
    {
    }

    template<class U>
    struct rebind
    {
        using other = monotonic_allocator<U>;
    };

    value_type* allocate(std::size_t n)
    {
        if (!std::align(
              alignof(T), sizeof(T) * n, pool_->position, pool_->size))
            throw std::bad_alloc{};
        auto p = reinterpret_cast<T*>(pool_->position);
        auto end = std::next(p, n);
        pool_->position = end;
        pool_->size -= sizeof(T) * n;
        return p;
    }

    void deallocate(value_type* p, std::size_t n)
    {
    }

    template<class U>
    friend struct monotonic_allocator;

private:
    monotonic_pool* pool_;
};

using fields_t = http::basic_fields<monotonic_allocator<char>>;
using request_t = http::request<http::empty_body, fields_t>;
using response_t = http::response<http::span_body<char const>, fields_t>;
using request_parser_t =
  http::request_parser<http::empty_body, monotonic_allocator<char>>;
using response_serializer_t = fastbeast::
  serializer<false, http::span_body<char const>, monotonic_allocator<char>>;

namespace
{

beast::string_view
mime_type(beast::string_view path)
{
    using beast::iequals;
    auto const ext = [&path] {
        auto const pos = path.rfind(".");
        if (pos == beast::string_view::npos)
            return beast::string_view{};
        return path.substr(pos);
    }();
    if (iequals(ext, ".htm"))
        return "text/html";
    if (iequals(ext, ".html"))
        return "text/html";
    if (iequals(ext, ".php"))
        return "text/html";
    if (iequals(ext, ".css"))
        return "text/css";
    if (iequals(ext, ".txt"))
        return "text/plain";
    if (iequals(ext, ".js"))
        return "application/javascript";
    if (iequals(ext, ".json"))
        return "application/json";
    if (iequals(ext, ".xml"))
        return "application/xml";
    if (iequals(ext, ".swf"))
        return "application/x-shockwave-flash";
    if (iequals(ext, ".flv"))
        return "video/x-flv";
    if (iequals(ext, ".png"))
        return "image/png";
    if (iequals(ext, ".jpe"))
        return "image/jpeg";
    if (iequals(ext, ".jpeg"))
        return "image/jpeg";
    if (iequals(ext, ".jpg"))
        return "image/jpeg";
    if (iequals(ext, ".gif"))
        return "image/gif";
    if (iequals(ext, ".bmp"))
        return "image/bmp";
    if (iequals(ext, ".ico"))
        return "image/vnd.microsoft.icon";
    if (iequals(ext, ".tiff"))
        return "image/tiff";
    if (iequals(ext, ".tif"))
        return "image/tiff";
    if (iequals(ext, ".svg"))
        return "image/svg+xml";
    if (iequals(ext, ".svgz"))
        return "image/svg+xml";
    return "application/text";
}

struct mmaped_file
{
    static mmaped_file open(char const* path, error_code& ec)
    {
        auto fd = ::open(path, O_RDONLY);
        if (fd == -1)
        {
            ec = {errno, boost::system::system_category()};
            return {};
        }

        struct stat sb = {};
        ::fstat(fd, &sb);
        mmaped_file f{::mmap(nullptr, sb.st_size, PROT_READ, MAP_SHARED, fd, 0),
                      sb.st_size};
        if (f.p_ == nullptr)
        {
            ec = {errno, boost::system::system_category()};
        }
        ::close(fd);
        return f;
    }

    mmaped_file() = default;

    mmaped_file(mmaped_file&& other) noexcept
      : p_{std::exchange(other.p_, nullptr)}
      , size_{std::exchange(other.size_, 0)}
    {
    }

    mmaped_file& operator=(mmaped_file&& other) noexcept
    {
        mmaped_file tmp{std::move(other)};
        std::swap(tmp.p_, p_);
        std::swap(tmp.size_, size_);
        return *this;
    }

    ~mmaped_file()
    {
        if (p_ != nullptr)
            ::munmap(p_, size_);
    }

    void const* data() const
    {
        return p_;
    }

    std::size_t size() const
    {
        return size_;
    }

    bool open() const
    {
        return p_ != nullptr;
    }

private:
    mmaped_file(void* p, std::size_t n)
      : p_{p}
      , size_{n}
    {
    }

    void* p_ = nullptr;
    std::size_t size_ = 0;
};

class file_cache
{
public:
    mmaped_file* get(std::string const& s)
    {
        auto& f = files_[s];
        if (f.open())
            return &f;
        error_code ec;
        f = mmaped_file::open(std::next(s.data()), ec);
        if (ec)
            return nullptr;
        return &f;
    }

private:
    std::unordered_map<std::string, mmaped_file> files_;
};

template<std::size_t N>
struct node_pool
{
    node_pool() = default;
    node_pool(node_pool&&) = delete;
    node_pool& operator=(node_pool&&) = delete;

    void* allocate()
    {
        if (pool_.empty())
            return ::operator new(N);
        else
        {
            auto* p = &pool_.back();
            pool_.pop_back();
            p->~list_base_hook();
            return p;
        }
    }

    void deallocate(void* p)
    {
        pool_.push_back(*::new (p) boost::intrusive::list_base_hook<>{});
    }

    ~node_pool()
    {
        pool_.clear_and_dispose([](auto* p) { ::operator delete(p); });
    }

    boost::intrusive::list<boost::intrusive::list_base_hook<>> pool_;
};

struct session
{
    static void* operator new(std::size_t n);
    static void operator delete(void*);

    explicit session(socket_t&& s)
      : socket{std::move(s)}
    {
    }

    static void read(std::unique_ptr<session>&& p,
                     error_code ec = {},
                     std::size_t n = 0)
    {
        auto& self = *p;
        if (ec)
        {
            self.socket.close();
            return;
        }

        self.parser.reset();
        self.serializer.reset();
        self.pool.reset();
        self.parser.emplace(
          std::piecewise_construct,
          std::make_tuple(),
          std::make_tuple(monotonic_allocator<char>{self.pool}));
        http::async_read(
          self.socket,
          self.buffer,
          *self.parser,
          beast::bind_front_handler(&process_request, std::move(p)));
    }

    static void send_error_response(std::unique_ptr<session>&& p,
                                    request_t const& req,
                                    http::status status,
                                    string_view body)
    {
        auto& self = *p;
        auto& response =
          self.response.emplace(std::piecewise_construct,
                                std::make_tuple(),
                                std::make_tuple(req.get_allocator()));
        auto& h = response.base();
        h.set(http::field::content_type, "application/text");
        h.set(http::field::server, "FastBeast");
        h.set(http::field::keep_alive, "keep-alive");
        h.result(status);
        response.body() = http::span_body<char const>::value_type{body};
        response.prepare_payload();
        auto& serializer = self.serializer.emplace(response);

        write(std::move(p));
    }

    static void send_file_response(std::unique_ptr<session>&& p,
                                   request_t const& req)
    {
        auto& self = *p;
        auto target = req.target();
        if (target.empty() || target[0] != '/' ||
            target.find("..") != std::string::npos)
            return send_error_response(
              std::move(p), req, http::status::not_found, "File not found\r\n");

        thread_local static file_cache cache;

        auto* file = cache.get(std::string{target});
        if (file == nullptr)
            return send_error_response(
              std::move(p), req, http::status::not_found, "File not found\r\n");

        auto& response =
          self.response.emplace(std::piecewise_construct,
                                std::make_tuple(),
                                std::make_tuple(req.get_allocator()));

        auto& h = response.base();
        h.set(http::field::content_type, mime_type(target));
        h.set(http::field::server, "FastBeast");
        h.set(http::field::keep_alive, "keep-alive");
        response.result(http::status::ok);
        response.body() = http::span_body<char const>::value_type{
          static_cast<char const*>(file->data()), file->size()};
        response.prepare_payload();

        auto& serializer = self.serializer.emplace(response);

        write(std::move(p));
    }

    static void write(std::unique_ptr<session>&& p,
                      error_code ec = {},
                      std::size_t n = 0)
    {
        auto& self = *p;
        if (ec)
        {
            self.socket.close();
            error_log() << "Write error: " << ec.message();
            return;
        }

        self.serializer->consume(n);

        auto buffers = self.serializer->next();
        if (std::all_of(
              buffers.begin(), buffers.end(), [](net::const_buffer const& b) {
                  return b.size() == 0;
              }))
        {
            read(std::move(p));
            return;
        }
        net::async_write(self.socket,
                         buffers,
                         beast::bind_front_handler(&write, std::move(p)));
    }

    static void process_request(std::unique_ptr<session>&& p,
                                error_code ec = {},
                                std::size_t n = 0)
    {
        auto& self = *p;
        if (ec)
        {
            self.socket.close();
            return;
        }

        auto const& req = self.parser->get();
        switch (req.method())
        {
            case http::verb::get:
                send_file_response(std::move(p), req);
                break;
            default:
            {
                send_error_response(std::move(p),
                                    req,
                                    http::status::bad_request,
                                    "Invalid request-method\r\n");
                break;
            }
        }
    }

    socket_t socket;
    std::optional<request_parser_t> parser;
    std::optional<response_t> response;
    std::optional<response_serializer_t> serializer;
    monotonic_pool pool;
    beast::flat_static_buffer<16384> buffer;
};

thread_local node_pool<sizeof(session)> session_pool;

void*
session::operator new(std::size_t n)
{
    if (n > sizeof(session))
        throw std::bad_alloc{};
    return session_pool.allocate();
}

void
session::operator delete(void* p)
{
    session_pool.deallocate(p);
}

} // namespace
void
spawn_http_session(socket_t&& sock)
{
    auto p = std::make_unique<session>(std::move(sock));
    session::read(std::move(p));
}

} // namespace fastbeast
