#ifndef FASTBEAST_FWD_HPP
#define FASTBEAST_FWD_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace fastbeast
{
namespace net = boost::asio;

using socket_t =
  net::basic_stream_socket<net::ip::tcp, net::io_context::executor_type>;
using acceptor_t =
  net::basic_socket_acceptor<net::ip::tcp, net::io_context::executor_type>;

} // namespace fastbeast

#endif // FASTBEAST_FWD_HPP
