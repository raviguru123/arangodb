////////////////////////////////////////////////////////////////////////////////
/// @brief resource holder for AQL queries with auto-free functionality
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_UTILS_AHUACATL_GUARD_H
#define TRIAGENS_UTILS_AHUACATL_GUARD_H 1

#include "Ahuacatl/ahuacatl-context.h"
#include "BasicsC/json.h"
#include "BasicsC/logging.h"
#include "VocBase/vocbase.h"

#ifdef TRI_ENABLE_CLUSTER
#include "Cluster/ServerState.h"
#endif

namespace triagens {
  namespace arango {

// -----------------------------------------------------------------------------
// --SECTION--                                               class AhuacatlGuard
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief scope guard for a TRI_aql_context_t*
////////////////////////////////////////////////////////////////////////////////

    class AhuacatlGuard {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief create the guard
////////////////////////////////////////////////////////////////////////////////

        AhuacatlGuard (TRI_vocbase_t* vocbase, 
                       const string& query,
                       TRI_json_t* userOptions) :
          _context(0) {
#ifdef TRI_ENABLE_CLUSTER
            const bool isCoordinator = ServerState::instance()->isCoordinator();
#else
            const bool isCoordinator = false;            
#endif            
            _context = TRI_CreateContextAql(vocbase, query.c_str(), query.size(), isCoordinator, userOptions);

            if (_context == 0) {
              LOG_DEBUG("failed to create context for query '%s'", query.c_str());
            }
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the guard
////////////////////////////////////////////////////////////////////////////////

        ~AhuacatlGuard () {
          this->free();
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief free the context
////////////////////////////////////////////////////////////////////////////////

        void free () {
          if (_context != 0) {
            TRI_FreeContextAql(_context);
            _context = 0;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief access the context
////////////////////////////////////////////////////////////////////////////////

        inline TRI_aql_context_t* ptr () const {
          return _context;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return whether context is valid
////////////////////////////////////////////////////////////////////////////////

        inline bool valid () const {
          return _context != 0;
        }

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup ArangoDB
/// @{
////////////////////////////////////////////////////////////////////////////////

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the AQL context C struct
////////////////////////////////////////////////////////////////////////////////

        TRI_aql_context_t* _context;

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

    };
  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
