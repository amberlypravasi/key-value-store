#pragma once

#include <string>

#include "kvstore/command.h"
#include "kvstore/store.h"

namespace kvstore {

// Runs `cmd` against `store` and returns the response to show the user, such
// as "OK", a value, "(nil)", or "ERROR: ...". Shared by the command line and
// the network server so they behave the same way.
//
// EXIT has no response; callers handle it by ending the session.
std::string handle_command(Store& store, const Command& cmd);

}  // namespace kvstore
