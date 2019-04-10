#ifndef FASTBEAST_LOG_HPP
#define FASTBEAST_LOG_HPP

#include <iosfwd>
#include <memory>
#include <sstream>

namespace fastbeast
{

struct log_line
{
public:
    explicit log_line(std::ostream& out)
      : out_{&out}
    {
    }

    template<class T>
    friend log_line& operator<<(log_line&& self, T const& t)
    {
        self.out_.get_deleter().line_ << t;
        return self;
    }

    template<class T>
    friend log_line& operator<<(log_line& self, T const& t)
    {
        self.out_.get_deleter().line_ << t;
        return self;
    }

private:
    struct deleter
    {
        void operator()(std::ostream* out)
        {
            line_ << '\n';
            *out << line_.rdbuf();
        }
        std::stringstream line_;
    };

    std::unique_ptr<std::ostream, deleter> out_;
};

log_line
error_log();

log_line
info_log();

} // namespace fastbeast

#endif // FASTBEAST_LOG_HPP
