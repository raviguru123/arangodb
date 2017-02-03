////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Manuel Baesler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_EXPORT_EXPORT_FEATURE_H
#define ARANGODB_EXPORT_EXPORT_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"
#include "V8Client/ArangoClientHelper.h"

namespace arangodb {
namespace httpclient {
class GeneralClientConnection;
class SimpleHttpClient;
class SimpleHttpResult;
}

class ExportFeature final : public application_features::ApplicationFeature,
                               public ArangoClientHelper {
 public:
  ExportFeature(application_features::ApplicationServer* server,
                   int* result);

 public:
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override;
  void validateOptions(
      std::shared_ptr<options::ProgramOptions> options) override;
  void start() override;

 private:
    void collectionExport(httpclient::SimpleHttpClient* httpClient);
    void graphExport(httpclient::SimpleHttpClient* httpClient);


 private:
  // ...

  int* _result;
};
}

#endif
