#ifndef FASTBEAST_RECYCLING_STACK_ALLOCATOR_HPP
#define FASTBEAST_RECYCLING_STACK_ALLOCATOR_HPP

#include <boost/context/protected_fixedsize_stack.hpp>
namespace fastbeast
{

template<class InnerAlloc>
class recycling_stack_allocator
{
public:
    using traits_type = typename InnerAlloc::traits_type;

    boost::context::stack_context allocate()
    {
        return get_state().allocate();
    }

    void deallocate(boost::context::stack_context& stack)
    {
        get_state().deallocate(stack);
    }

private:
    struct state
    {
        state()
        {
            stacks_.reserve(128);
        }

        InnerAlloc alloc_;
        std::vector<boost::context::stack_context> stacks_;

        boost::context::stack_context allocate()
        {
            if (stacks_.empty())
            {
                return alloc_.allocate();
            }

            auto stack = stacks_.back();
            stacks_.pop_back();
            return stack;
        }

        void deallocate(boost::context::stack_context& stack)
        {
            try
            {
                stacks_.push_back(stack);
            }
            catch (...)
            {
                alloc_.deallocate(stack);
            }
        }

        ~state()
        {
            for (auto stack : stacks_)
                alloc_.deallocate(stack);
        }
    };

    static state& get_state()
    {
        thread_local static state s;
        return s;
    }
};

} // namespace fastbeast

#endif // FASTBEAST_RECYCLING_STACK_ALLOCATOR_HPP
