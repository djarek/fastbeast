#include "http_session.hpp"
#include "log.hpp"
#include "recycling_stack_allocator.hpp"

#include <boost/asio/basic_socket_acceptor.hpp>
#include <boost/asio/io_context.hpp>
#include <ufiber/ufiber.hpp>

#include <algorithm>
#include <thread>

namespace fastbeast
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

using yield_t = ufiber::yield_token<net::io_context::executor_type>;

void
accept(acceptor_t&& a, std::function<void(socket_t&& sock)> fn)
{
    ufiber::spawn(
      std::allocator_arg,
      recycling_stack_allocator<boost::context::fixedsize_stack>{},
      a.get_executor(),
      [a = std::move(a), fn = std::move(fn)](yield_t yield) mutable {
          socket_t socket{a.get_executor()};
          while (true)
          {
              if (auto ec = a.async_accept(socket, yield); ec)
              {
                  error_log() << "Accept error: " << ec.message();
                  break;
              }
              fn(std::move(socket));
          }
      });
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
            fastbeast::accept(fastbeast::make_acceptor(io, addr, 8080),
                              &spawn_http_session);
            io.run();
        }};
    });

    net::io_context main{1};
    fastbeast::accept(fastbeast::make_acceptor(main, addr, 8080),
                      &spawn_http_session);
    main.run();
    for (auto& t : threads)
        t.join();
}

} // namespace fastbeast

int
main()
{
    fastbeast::run();
}
