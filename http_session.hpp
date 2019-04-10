#ifndef FASTBEAST_HTTP_SESSION_HPP
#define FASTBEAST_HTTP_SESSION_HPP

#include "fwd.hpp"

namespace fastbeast
{

void
spawn_http_session(socket_t&& sock);

} // namespace fastbeast

#endif // FASTBEAST_HTTP_SESSION_HPP
