#include "http_session.hpp"
#include "log.hpp"

#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/bind_handler.hpp>

#include <algorithm>
#include <thread>

namespace fastbeast
{
namespace
{

struct reuse_port
{
    template<class Protocol>
    static int level(Protocol&& /*p*/)
    {
        return SOL_SOCKET;
    }

    template<class Protocol>
    static int name(Protocol&& /*p*/)
    {
        return SO_REUSEPORT;
    }

    template<class Protocol>
    void const* data(Protocol&& /*p*/) const
    {
        return &value_;
    }

    template<class Protocol>
    static std::size_t size(Protocol&& /*p*/)
    {
        return sizeof(value_);
    }

    explicit reuse_port(bool v = true)
      : value_{v}
    {
    }

    int value_;
};

struct acceptor_context
{
    static void accept(std::unique_ptr<acceptor_context>&& p,
                       boost::system::error_code const& ec = {})
    {
        auto& self = *p;
        if (ec)
        {
            error_log() << "Accept error: " << ec.message();
            return;
        }

        spawn_http_session(std::move(self.socket));

        self.acceptor.async_accept(
          self.socket, boost::beast::bind_front_handler(&accept, std::move(p)));
    }

    explicit acceptor_context(acceptor_t&& a)
      : acceptor{std::move(a)}
    {
    }

    acceptor_t acceptor;
    socket_t socket{acceptor.get_executor()};
};

void
accept(acceptor_t&& a)
{
    auto p = std::make_unique<acceptor_context>(std::move(a));
    p->accept(std::move(p));
}

acceptor_t
make_acceptor(net::io_context& io, net::ip::address addr, std::uint16_t port)
{
    acceptor_t ret{io};
    net::ip::tcp::endpoint ep{addr, port};
    ret.open(ep.protocol());
    ret.set_option(fastbeast::reuse_port{true});
    ret.bind(ep);
    ret.listen();
    return ret;
}

void
run()
{
    auto addr = net::ip::make_address("0.0.0.0");
    std::vector<std::thread> threads;
    threads.reserve(0.5 * std::thread::hardware_concurrency() - 1);
    std::generate_n(std::back_inserter(threads), threads.capacity(), [&]() {
        return std::thread{[addr]() {
            net::io_context io{1};
            fastbeast::accept(fastbeast::make_acceptor(io, addr, 8080));
            io.run();
        }};
    });

    net::io_context main{1};
    fastbeast::accept(fastbeast::make_acceptor(main, addr, 8080));
    main.run();
    for (auto& t : threads)
        t.join();
}

} // namespace
} // namespace fastbeast

int
main()
{
    fastbeast::run();
}
