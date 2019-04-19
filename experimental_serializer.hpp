#ifndef FASTBEAST_EXPERIMENTAL_SERIALIZER_HPP
#define FASTBEAST_EXPERIMENTAL_SERIALIZER_HPP

#include <boost/asio/buffer.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/span_body.hpp>
#include <vector>

namespace fastbeast
{
namespace net = boost::asio;
namespace http = boost::beast::http;
using string_view = boost::beast::string_view;

struct buffer_sequence_view
{
    explicit buffer_sequence_view(net::const_buffer const* first,
                                  net::const_buffer const* last)
      : first_(first)
      , last_(last)
    {
    }

    net::const_buffer const* begin() const
    {
        return first_;
    }

    net::const_buffer const* end() const
    {
        return last_;
    }

private:
    net::const_buffer const* first_;
    net::const_buffer const* last_;
};

struct header_serializer
{
    template<class Allocator>
    header_serializer(http::basic_fields<Allocator> const& fields,
                      unsigned version,
                      http::verb verb)
      : is_request_(true)
    {
        if (verb == http::verb::unknown)
            sv_ = fields.get_method_impl();
        else
            sv_ = http::to_string(verb);

        // target_or_reason_ has a leading SP

        buf_[0] = ' ';
        buf_[1] = 'H';
        buf_[2] = 'T';
        buf_[3] = 'T';
        buf_[4] = 'P';
        buf_[5] = '/';
        buf_[6] = '0' + static_cast<char>(version / 10);
        buf_[7] = '.';
        buf_[8] = '0' + static_cast<char>(version % 10);
        buf_[9] = '\r';
        buf_[10] = '\n';
        buf_len_ = 11;
    }

    template<class Allocator>
    header_serializer(http::basic_fields<Allocator> const& fields,
                      unsigned version,
                      unsigned code)
      : is_request_(false)
    {
        buf_[0] = 'H';
        buf_[1] = 'T';
        buf_[2] = 'T';
        buf_[3] = 'P';
        buf_[4] = '/';
        buf_[5] = '0' + static_cast<char>(version / 10);
        buf_[6] = '.';
        buf_[7] = '0' + static_cast<char>(version % 10);
        buf_[8] = ' ';
        buf_[9] = '0' + static_cast<char>(code / 100);
        buf_[10] = '0' + static_cast<char>((code / 10) % 10);
        buf_[11] = '0' + static_cast<char>(code % 10);
        buf_[12] = ' ';
        buf_len_ = 13;

        if (!fields.get_reason_impl().empty())
            sv_ = fields.get_reason_impl();
        else
            sv_ = http::obsolete_reason(static_cast<http::status>(code));
    }

    template<class Allocator>
    bool fill(std::vector<net::const_buffer, Allocator>& buffers,
              http::basic_fields<Allocator> const& fields)
    {
        buffers.reserve(64);
        if (is_request_)
        {
            buffers.push_back(net::const_buffer(sv_.data(), sv_.size()));
            buffers.push_back(
              net::const_buffer{fields.get_method_impl().data(),
                                fields.get_method_impl().size()});
            buffers.push_back(net::const_buffer(buf_, buf_len_));
        }
        else
        {
            buffers.push_back(net::const_buffer(buf_, buf_len_));
            buffers.push_back(net::const_buffer(sv_.data(), sv_.size()));
            buffers.push_back(http::detail::chunk_crlf());
        }

        std::transform(
          fields.begin(),
          fields.end(),
          std::back_inserter(buffers),
          [](typename http::basic_fields<Allocator>::value_type const& value) {
              return value.buffer();
          });

        buffers.push_back(http::detail::chunk_crlf());

        return false;
    }

private:
    string_view sv_;
    std::size_t buf_len_ = 0;
    char buf_[13];
    bool is_request_;
};

template<class Body>
struct body_serializer;

template<class T>
struct body_serializer<http::span_body<T>>
{
    template<class BuffersVector, class Message>
    body_serializer(BuffersVector& /*buffers*/, Message const& /*msg*/)
    {
    }

    template<class BuffersVector, class Message>
    bool fill(BuffersVector& buffers, Message const& msg)
    {
        auto span = msg.body();
        buffers.push_back(net::const_buffer{span.data(), span.size()});
        return false;
    }
};

template<bool isRequest, class Body, class Allocator>
class serializer
{
    using body_serializer_t = body_serializer<Body>;

public:
    explicit serializer(
      http::message<isRequest, Body, http::basic_fields<Allocator>>& msg)
      : msg_{msg}
      , buffers_{msg_.base().get_allocator()}
    {
    }

    buffer_sequence_view next()
    {
        auto result = [&]() {
            return buffer_sequence_view{buffers_.data(),
                                        buffers_.data() + buffers_.size()};
        };

        BOOST_ASIO_CORO_REENTER(coro_)
        {
            construct_header_serializer();
            while (true)
            {
                if (!header_serializer_->fill(buffers_, msg_.base()))
                {
                    break;
                }
                while (!buffers_.empty())
                {
                    BOOST_ASIO_CORO_YIELD return result();
                }
            }

            if (split_)
            {
                while (split_ || !buffers_.empty())
                {
                    header_done_ = buffers_.empty();
                    BOOST_ASIO_CORO_YIELD return result();
                }
            }
            header_done_ = true;
            construct_body_serializer();
            while (true)
            {
                if (!body_serializer_->fill(buffers_, msg_))
                {
                    break;
                }
                while (!buffers_.empty())
                {
                    BOOST_ASIO_CORO_YIELD return result();
                }
            }

            while (!buffers_.empty())
                BOOST_ASIO_CORO_YIELD return result();
            return result();
        }

        __builtin_unreachable();
    }

    void consume(std::size_t n)
    {
        auto it = std::find_if(
          buffers_.begin(), buffers_.end(), [&n](net::const_buffer const& b) {
              if (n < b.size())
                  return true;

              n -= b.size();
              return false;
          });

        if (it != buffers_.end())
            (*it) += n;

        buffers_.erase(buffers_.begin(), it);
    }

private:
    void construct_header_serializer()
    {
        if constexpr (isRequest)
        {
            header_serializer_.emplace(msg_, msg_.version(), msg_.method());
        }
        else
        {
            header_serializer_.emplace(msg_, msg_.version(), msg_.result_int());
        }
    }

    void construct_body_serializer()
    {
        body_serializer_.emplace(msg_.base(), msg_.body());
    }

    http::message<isRequest, Body, http::basic_fields<Allocator>>& msg_;
    std::vector<net::const_buffer, Allocator> buffers_;
    boost::optional<header_serializer> header_serializer_;
    boost::optional<body_serializer_t> body_serializer_;
    std::size_t limit_ = std::numeric_limits<std::size_t>::max();
    net::coroutine coro_;
    bool split_ = false;
    bool header_done_ = false;
};

} // namespace fastbeast

#endif // FASTBEAST_EXPERIMENTAL_SERIALIZER_HPP
