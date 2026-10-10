#pragma once
#include <string>

namespace NavalCommand::Gateway::Grpc::Api
{
	int ReloadSmoke(const std::string& endpoint);
	int RouteSmoke(const std::string& endpoint, bool blocked);
}
