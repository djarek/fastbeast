#include "http_session.hpp"
#include "log.hpp"
#include "recycling_stack_allocator.hpp"

#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/http/read.hpp>
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

namespace
{

struct request
{
    int version;
    http::verb verb;
    std::string target;
    bool keep_alive = true;
};

struct response_header
{
    string_view content_type;
    std::size_t content_length = 0;
    http::status status;
    int version;
    bool keep_alive;
};

struct lean_parser : http::basic_parser<true>
{
    void on_request_impl(http::verb verb,
                         string_view /*method_str*/,
                         string_view target,
                         int version,
                         error_code& ec) override
    {
        if (target.size() >= request_.target.max_size())
        {
            ec = http::error::bad_target;
            return;
        }

        request_.target = {target.data(), target.size()};
        request_.version = version;
        request_.verb = verb;
    }

    void on_response_impl(int, string_view, int, error_code&) override
    {
        __builtin_unreachable();
    }

    void on_field_impl(http::field /*f*/,
                       string_view /*name*/,
                       string_view /*value*/,
                       error_code&) override
    {
    }

    void on_header_impl(error_code& ec) override
    {
        ec = {};
        request_.keep_alive = this->keep_alive();
    }

    void on_body_init_impl(boost::optional<std::uint64_t> const&,
                           error_code& ec) override
    {
        ec = {};
    }

    std::size_t on_body_impl(string_view s, error_code&) override
    {
        return s.size();
    }

    void on_chunk_header_impl(std::uint64_t,
                              string_view,
                              error_code& ec) override
    {
        ec = {};
    }

    std::size_t on_chunk_body_impl(std::uint64_t,
                                   string_view s,
                                   error_code& ec) override
    {
        ec = {};
        return s.size();
    }

    void on_finish_impl(error_code& ec) override
    {
        ec = {};
    }

    request request_;
};

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
        return "Content-Type: text/html\r\n";
    if (iequals(ext, ".html"))
        return "Content-Type: text/html\r\n";
    if (iequals(ext, ".php"))
        return "Content-Type: text/html\r\n";
    if (iequals(ext, ".css"))
        return "Content-Type: text/css\r\n";
    if (iequals(ext, ".txt"))
        return "Content-Type: text/plain\r\n";
    if (iequals(ext, ".js"))
        return "Content-Type: application/javascript\r\n";
    if (iequals(ext, ".json"))
        return "Content-Type: application/json\r\n";
    if (iequals(ext, ".xml"))
        return "Content-Type: application/xml\r\n";
    if (iequals(ext, ".swf"))
        return "Content-Type: application/x-shockwave-flash\r\n";
    if (iequals(ext, ".flv"))
        return "Content-Type: video/x-flv\r\n";
    if (iequals(ext, ".png"))
        return "Content-Type: image/png\r\n";
    if (iequals(ext, ".jpe"))
        return "Content-Type: image/jpeg\r\n";
    if (iequals(ext, ".jpeg"))
        return "Content-Type: image/jpeg\r\n";
    if (iequals(ext, ".jpg"))
        return "Content-Type: image/jpeg\r\n";
    if (iequals(ext, ".gif"))
        return "Content-Type: image/gif\r\n";
    if (iequals(ext, ".bmp"))
        return "Content-Type: image/bmp\r\n";
    if (iequals(ext, ".ico"))
        return "Content-Type: image/vnd.microsoft.icon\r\n";
    if (iequals(ext, ".tiff"))
        return "Content-Type: image/tiff\r\n";
    if (iequals(ext, ".tif"))
        return "Content-Type: image/tiff\r\n";
    if (iequals(ext, ".svg"))
        return "Content-Type: image/svg+xml\r\n";
    if (iequals(ext, ".svgz"))
        return "Content-Type: image/svg+xml\r\n";
    return "Content-Type: application/text\r\n";
}

template<std::size_t N>
net::const_buffer
strbuf(char const (&b)[N])
{
    return net::const_buffer{b, N - 1};
}

void
send_response(socket_t& s,
              response_header const& h,
              net::const_buffer body,
              yield_t& yield)
{
    beast::static_string<32> resp_line;

    switch (h.version)
    {
        case 10:
            resp_line += "HTTP/1.0 ";
            break;
        case 11:
            resp_line += "HTTP/1.1 ";
            break;
    }

    resp_line += beast::to_static_string(static_cast<std::uint64_t>(h.status));
    resp_line += ' ';
    resp_line += http::obsolete_reason(h.status);
    resp_line += "\r\n";

    auto const len_str =
      beast::to_static_string(static_cast<std::uint64_t>(h.content_length));

    net::const_buffer keep_alive;
    if (h.keep_alive)
        keep_alive = strbuf("Connection: keep-alive\r\n");
    else
        keep_alive = strbuf("Connection: close\r\n");

    std::array<net::const_buffer, 8> buffers{
      net::const_buffer{resp_line.data(), resp_line.size()},
      strbuf("Server: FastBeast\r\n"),
      net::const_buffer{h.content_type.data(), h.content_type.size()},
      keep_alive,
      strbuf("Content-Length: "),
      net::const_buffer{len_str.data(), len_str.size()},
      strbuf("\r\n\r\n"),
      body};
    if (auto [ec, n] = net::async_write(s, buffers, yield); ec)
    {
        error_log() << "HTTP response write error: " << ec.message();
        return;
    }
}

void
send_error_response(socket_t& s,
                    request const& r,
                    http::status status,
                    string_view resp,
                    yield_t& yield)
{
    response_header h;
    h.status = status;
    h.content_length = resp.length();
    h.content_type = "Content-Type: application/text\r\n";
    h.keep_alive = r.keep_alive;
    h.version = r.version;

    send_response(s, h, net::const_buffer(resp.data(), resp.size()), yield);
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
send_file_response(socket_t& s, request const& r, yield_t& yield)
{
    if (r.target.empty() || r.target[0] != '/' ||
        r.target.find("..") != std::string::npos)
    {
        send_error_response(
          s, r, http::status::not_found, "File not found\r\n", yield);
        return;
    }

    thread_local static file_cache cache;

    auto* file = cache.get(r.target);
    if (file == nullptr)
    {
        send_error_response(
          s, r, http::status::not_found, "File not found\r\n", yield);
        return;
    }

    response_header h;
    h.status = http::status::ok;
    h.content_length = file->size();
    h.content_type = mime_type(r.target);
    h.keep_alive = r.keep_alive;
    h.version = r.version;

    send_response(s, h, net::const_buffer{file->data(), file->size()}, yield);
}

void
process_request(socket_t& s, request const& req, yield_t& yield)
{
    switch (req.verb)
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
              lean_parser parser;
              parser.body_limit(0);

              if (auto [ec, n] = http::async_read(sock, buffer, parser, yield);
                  ec)
              {
                  error_log() << "HTTP read error: " << ec.message();
                  sock.close(ec);
                  break;
              }

              process_request(sock, parser.request_, yield);
          }
      });
}

} // namespace fastbeast
