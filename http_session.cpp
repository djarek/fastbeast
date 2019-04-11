#include "http_session.hpp"
#include "log.hpp"
#include "recycling_stack_allocator.hpp"

#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/span_body.hpp>
#include <boost/beast/http/write.hpp>
#include <ufiber/ufiber.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace fastbeast
{
using yield_t = ufiber::yield_token<net::io_context::executor_type>;

namespace http = boost::beast::http;
namespace beast = boost::beast;
using error_code = boost::system::error_code;
using string_view = beast::string_view;

struct monotonic_pool
{
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
    explicit monotonic_allocator(monotonic_allocator<U> const& other)
      : pool_{other.pool_}
    {
    }

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
using response_serializer_t =
  http::response_serializer<http::span_body<char const>, fields_t>;

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

void
send_error_response(socket_t& s,
                    request_t const& r,
                    http::status status,
                    string_view body,
                    yield_t& yield)
{
    response_t resp{std::piecewise_construct,
                    std::make_tuple(),
                    std::make_tuple(r.get_allocator())};
    auto& h = resp.base();
    h.set(http::field::content_type, "application/text");
    h.set(http::field::server, "FastBeast");
    h.set(http::field::keep_alive, "keep-alive");
    h.result(http::status::ok);
    resp.body() = http::span_body<char const>::value_type{body};
    resp.prepare_payload();

    response_serializer_t serializer{resp};
    if (auto [ec, n] = http::async_write(s, serializer, yield); ec)
        error_log() << "Write error: " << ec.message();
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

void
send_file_response(socket_t& s, request_t const& r, yield_t& yield)
{
    auto target = r.target();
    if (target.empty() || target[0] != '/' ||
        target.find("..") != std::string::npos)
    {
        send_error_response(
          s, r, http::status::not_found, "File not found\r\n", yield);
        return;
    }

    thread_local static file_cache cache;

    auto* file = cache.get(std::string{target});
    if (file == nullptr)
    {
        send_error_response(
          s, r, http::status::not_found, "File not found\r\n", yield);
        return;
    }

    response_t resp{std::piecewise_construct,
                    std::make_tuple(),
                    std::make_tuple(r.get_allocator())};
    auto& h = resp.base();
    h.set(http::field::content_type, mime_type(r.target()));
    h.set(http::field::server, "FastBeast");
    h.set(http::field::keep_alive, "keep-alive");
    resp.result(http::status::ok);
    resp.body() = http::span_body<char const>::value_type{
      static_cast<char const*>(file->data()), file->size()};
    resp.prepare_payload();

    response_serializer_t serializer{resp};
    if (auto [ec, n] = http::async_write(s, serializer, yield); ec)
        error_log() << "Write error: " << ec.message();
}

void
process_request(socket_t& s, request_t const& req, yield_t& yield)
{
    switch (req.method())
    {
        case http::verb::get:
            send_file_response(s, req, yield);
            break;
        default:
        {
            send_error_response(s,
                                req,
                                http::status::bad_request,
                                "Invalid request-method\r\n",
                                yield);
            break;
        }
    }
}

} // namespace
void
spawn_http_session(socket_t&& sock)
{
    ufiber::spawn(
      std::allocator_arg,
      recycling_stack_allocator<boost::context::fixedsize_stack>{},
      sock.get_executor(),
      [sock = std::move(sock)](yield_t yield) mutable {
          beast::flat_static_buffer<16384> buffer;

          while (true)
          {
              monotonic_pool pool;
              request_parser_t parser{
                std::piecewise_construct,
                std::make_tuple(),
                std::make_tuple(monotonic_allocator<char>{pool})};
              parser.header_limit(8192);
              if (auto [ec, n] = http::async_read(sock, buffer, parser, yield);
                  ec)
              {
                  error_log() << "HTTP read error: " << ec.message();
                  sock.close(ec);
                  break;
              }

              process_request(sock, parser.release(), yield);
              if (!parser.keep_alive())
              {
                  sock.close();
                  return;
              }
          }
      });
}

} // namespace fastbeast
