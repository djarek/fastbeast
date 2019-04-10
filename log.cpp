#include "log.hpp"

#include <iostream>

namespace fastbeast
{

log_line
error_log()
{
    return log_line{std::clog};
}

log_line
info_log()
{
    return log_line{std::cout};
}

} // namespace fastbeast
