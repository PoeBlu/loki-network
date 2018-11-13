#ifndef LLARP_EXIT_CONTEXT_HPP
#define LLARP_EXIT_CONTEXT_HPP
#include <llarp/exit/policy.hpp>
#include <string>
#include <unordered_map>
#include <llarp/handlers/exit.hpp>
#include <llarp/router.h>

namespace llarp
{
  namespace exit
  {
    /// owner of all the exit endpoints
    struct Context
    {
      using Config_t = std::unordered_multimap< std::string, std::string >;

      Context(llarp_router *r);
      ~Context();

      void
      Tick(llarp_time_t now);

      bool
      AddExitEndpoint(const std::string &name, const Config_t &config);

     private:
      llarp_router *m_Router;
      std::unordered_map< std::string,
                          std::unique_ptr< llarp::handlers::ExitEndpoint > >
          m_Exits;
    };
  }  // namespace exit
}  // namespace llarp

#endif