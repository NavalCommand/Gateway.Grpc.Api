#include <NavalCommand/Gateway/Grpc/Api/LaunchSmoke.h>
#include <NavalCommand/Gateway/Grpc/Api/ManualSmoke.h>
#include <NavalCommand/Gateway/Grpc/Api/MovementSmoke.h>
#include <chrono>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <string>

using namespace navalcommand::gateway::v1;

namespace
{
	constexpr auto RPC_TIMEOUT = std::chrono::seconds(3);

	template <class T>
	void Print(const char* label, const T& value)
	{
		std::string jsonText;
		google::protobuf::util::MessageToJsonString(value, &jsonText);
		std::cout << label << " " << jsonText << std::endl;
	}
}

int main(int argc, char** argv)
{
	std::string endpoint = "127.0.0.1:50051";
	bool smoke = false;
	bool blocked = false;
	bool reload = false;
	bool movement = false;
	bool launch = false;
	for (int pollIndex = 1; pollIndex < argc; ++pollIndex)
	{
		std::string argument = argv[pollIndex];
		if (argument == "--schema")
		{
			std::cout << WorldSnapshot::descriptor()->full_name() << " " << Gateway::service_full_name() << std::endl;
			return 0;
		}
		if (argument == "--endpoint" && pollIndex + 1 < argc)
		{
			endpoint = argv[++pollIndex];
		}
		else if (argument == "--smoke")
		{
			smoke = true;
		}
		else if (argument == "--blocked")
		{
			blocked = true;
		}
		else if (argument == "--reload")
		{
			reload = true;
		}
		else if (argument == "--movement-smoke")
		{
			movement = true;
		}
		else if (argument == "--launch-smoke")
		{
			launch = true;
		}
		else if (argument != "--endpoint")
		{
			std::cerr << "Usage: naval-cli [--endpoint 127.0.0.1:port] "
				"[--smoke|--blocked|--reload|--movement-smoke|--launch-smoke] (default read-only)\n";
			return 2;
		}
	}
	if (!endpoint.starts_with("127.0.0.1:"))
	{
		return 2;
	}
	if (movement)
	{
		return MovementSmoke(endpoint);
	}
	if (launch)
	{
		return LaunchSmoke(endpoint);
	}
	if (reload)
	{
		return NavalCommand::Gateway::Grpc::Api::ReloadSmoke(endpoint);
	}
	if (smoke || blocked)
	{
		return NavalCommand::Gateway::Grpc::Api::RouteSmoke(endpoint, blocked);
	}

	auto stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
	WorldSnapshot snapshot;
	if (!stub->GetView(&context, Query{}, &snapshot).ok())
	{
		std::cerr << "View unavailable\n";
		return 3;
	}
	Print("view", snapshot);
	return 0;
}
