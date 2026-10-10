#include <NavalCommand/Gateway/Grpc/Api/LaunchSmoke.h>
#include <NavalCommand/Gateway/Grpc/Api/MovementSmoke.h>
#include <chrono>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <string>
#include <thread>
using namespace navalcommand::gateway::v1;
namespace {
constexpr auto RPC_TIMEOUT = std::chrono::seconds(3);
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(250);
constexpr int MAXIMUM_STATUS_POLLS = 240;
constexpr int MAXIMUM_PROTECTED_STATE_POLLS = 100;
constexpr double WAYPOINT_LATITUDE_OFFSET_DEGREES = 0.03;
constexpr double MINIMUM_DISPLACEMENT_METERS = 5;
} // namespace
template <class T> void Print(const char *label, const T &value) {
	std::string jsonText;
	google::protobuf::util::MessageToJsonString(value, &jsonText);
	std::cout << label << " " << jsonText << std::endl;
}
int main(int argc, char **argv) {
	std::string endpoint = "127.0.0.1:50051";
	bool smoke = false;
	bool blocked = false;
	bool reload = false;
	bool movement = false;
	bool launch = false;
	for (int pollIndex = 1; pollIndex < argc; ++pollIndex) {
		std::string argument = argv[pollIndex];
		if (argument == "--schema") {
			std::cout << WorldSnapshot::descriptor()->full_name() << " " << Gateway::service_full_name() << std::endl;
			return 0;
		}
		if (argument == "--endpoint" && pollIndex + 1 < argc) {
			endpoint = argv[++pollIndex];
		} else if (argument == "--smoke") {
			smoke = true;
		} else if (argument == "--blocked") {
			blocked = true;
		} else if (argument == "--reload") {
			reload = true;
		} else if (argument == "--movement-smoke") {
			movement = true;
		} else if (argument == "--launch-smoke") {
			launch = true;
		} else if (argument != "--endpoint") {
			std::cerr << "Usage: naval-cli [--endpoint 127.0.0.1:port] "
						 "[--smoke|--blocked|--reload|--movement-smoke|--launch-smoke] (default read-only)\n";
			return 2;
		}
	}
	if (!endpoint.starts_with("127.0.0.1:")) {
		return 2;
	}
	if (movement) {
		return MovementSmoke(endpoint);
	}
	if (launch) {
		return LaunchSmoke(endpoint);
	}
	auto stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto query = [&](WorldSnapshot &snapshot) {
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		return stub->GetView(&context, Query{}, &snapshot).ok();
	};
	WorldSnapshot snapshot;
	if (!query(snapshot)) {
		std::cerr << "View unavailable\n";
		return 3;
	}
	Print("view", snapshot);
	if (reload) {
		const auto originalSnapshot = snapshot;
		bool changed = false;
		for (int pollIndex = 0; pollIndex < MAXIMUM_STATUS_POLLS; ++pollIndex) {
			std::this_thread::sleep_for(POLL_INTERVAL);
			WorldSnapshot nextSnapshot;
			if (query(nextSnapshot) && nextSnapshot.session().session_id() != originalSnapshot.session().session_id()) {
				snapshot = nextSnapshot;
				changed = true;
				break;
			}
		}
		if (!changed || originalSnapshot.own_units().empty() || snapshot.own_units().empty()) {
			return 12;
		}
		OrderRequest stale;
		stale.set_session_id(originalSnapshot.session().session_id());
		stale.set_request_id("reload-stale");
		stale.set_entity_id(originalSnapshot.own_units(0).id());
		stale.set_based_on_version(originalSnapshot.observation_version());
		stale.mutable_waypoint()->set_latitude_deg(originalSnapshot.own_units(0).position().latitude_deg() + WAYPOINT_LATITUDE_OFFSET_DEGREES);
		stale.mutable_waypoint()->set_longitude_deg(originalSnapshot.own_units(0).position().longitude_deg());
		OrderResult rejected;
		{
			grpc::ClientContext context;
			if (!stub->SubmitOrder(&context, stale, &rejected).ok()) {
				return 13;
			}
		}
		Print("old-session-rejected", rejected);
		if (rejected.stage() != REJECTED || rejected.reason() != "stale_session") {
			return 14;
		}
		stale.set_session_id(snapshot.session().session_id());
		stale.set_request_id("reload-old-handle");
		stale.set_based_on_version(snapshot.observation_version());
		{
			grpc::ClientContext context;
			if (!stub->SubmitOrder(&context, stale, &rejected).ok()) {
				return 15;
			}
		}
		Print("old-handle-rejected", rejected);
		if (rejected.reason() != "wrong_side_or_invalid_entity") {
			return 16;
		}
		Print("reloaded-view", snapshot);
		std::cout << "RELOAD_PASS" << std::endl;
		return 0;
	}
	if (!smoke && !blocked) {
		return 0;
	}
	if (snapshot.own_units().empty()) {
		return 4;
	}
	const Unit *selectedUnit = nullptr;
	for (int attempt = 0; attempt < (blocked ? MAXIMUM_PROTECTED_STATE_POLLS : 1); ++attempt) {
		for (auto &unit : snapshot.own_units()) {
			for (auto &eligibility : unit.orders()) {
				if (eligibility.availability() == (blocked ? BLOCKED : EXECUTABLE) && (!blocked || eligibility.reason() == "protected_native_action") &&
					eligibility.kind() == "set_waypoint") {
					selectedUnit = &unit;
					break;
				}
			}
		}
		if (selectedUnit) {
			break;
		}
		if (blocked) {
			std::this_thread::sleep_for(POLL_INTERVAL);
			if (!query(snapshot)) {
				break;
			}
		}
	}
	if (!selectedUnit) {
		std::cerr << "Requested eligible example unavailable\n";
		return 5;
	}
	Query discoveryQuery;
	discoveryQuery.set_session_id(snapshot.session().session_id());
	discoveryQuery.set_entity_id(selectedUnit->id());
	OrderDiscovery discovery;
	{
		grpc::ClientContext context;
		if (!stub->DiscoverOrders(&context, discoveryQuery, &discovery).ok()) {
			return 6;
		}
	}
	Print("discovery", discovery);
	OrderRequest waypointRequest;
	waypointRequest.set_session_id(snapshot.session().session_id());
	waypointRequest.set_entity_id(selectedUnit->id());
	waypointRequest.set_based_on_version(snapshot.observation_version());
	waypointRequest.set_request_id("smoke-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
	waypointRequest.mutable_waypoint()->set_latitude_deg(selectedUnit->position().latitude_deg() + WAYPOINT_LATITUDE_OFFSET_DEGREES);
	waypointRequest.mutable_waypoint()->set_longitude_deg(selectedUnit->position().longitude_deg());
	auto submit = [&](const OrderRequest &request) {
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		OrderResult result;
		if (!stub->SubmitOrder(&context, request, &result).ok()) {
			result.set_reason("rpc_failed");
		}
		Print("submit", result);
		return result;
	};
	auto wait = [&](const std::string &requestId, bool requireMovement) {
		OrderResult result;
		for (int pollIndex = 0; pollIndex < MAXIMUM_STATUS_POLLS; ++pollIndex) {
			Query statusQuery;
			statusQuery.set_session_id(waypointRequest.session_id());
			statusQuery.set_request_id(requestId);
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			auto rpcStatus = stub->GetStatus(&context, statusQuery, &result);
			if (!rpcStatus.ok()) {
				break;
			}
			if (result.stage() == REJECTED || result.stage() == MOVEMENT_OBSERVED || (!requireMovement && result.stage() == NATIVE_TASK_CREATED)) {
				break;
			}
			std::this_thread::sleep_for(POLL_INTERVAL);
		}
		Print("status", result);
		return result;
	};
	if (blocked) {
		submit(waypointRequest);
		if (wait(waypointRequest.request_id(), false).reason() != "protected_native_action") {
			return 7;
		}
		for (auto mode : {REPLACE_ROUTE, APPEND_WAYPOINT}) {
			waypointRequest.set_request_id(waypointRequest.request_id() + "-" + std::to_string(mode));
			waypointRequest.mutable_waypoint()->set_route_mode(mode);
			submit(waypointRequest);
			if (wait(waypointRequest.request_id(), false).reason() != "protected_native_action") {
				return 7;
			}
		}
		WorldSnapshot protectedView;
		if (!query(protectedView)) {
			return 7;
		}
		Print("protected-after", protectedView);
		return 0;
	}
	int invalidRequestIndex = 0;
	for (auto reason : {"stale_session", "wrong_side_or_invalid_entity", "invalid_arguments", "stale_observation"}) {
		auto invalidRequest = waypointRequest;
		invalidRequest.set_request_id(waypointRequest.request_id() + "-bad" + std::to_string(invalidRequestIndex++));
		std::string expectedReason = reason;
		if (expectedReason == "stale_session") {
			invalidRequest.set_session_id("old");
		}
		if (expectedReason == "wrong_side_or_invalid_entity") {
			invalidRequest.set_entity_id("opponent-does-not-exist");
		}
		if (expectedReason == "invalid_arguments") {
			invalidRequest.mutable_waypoint()->set_latitude_deg(100);
		}
		if (expectedReason == "stale_observation") {
			invalidRequest.set_based_on_version(0);
		}
		auto result = submit(invalidRequest);
		if (result.stage() != REJECTED) {
			result = wait(invalidRequest.request_id(), false);
		}
		if (result.stage() != REJECTED || result.reason() != reason) {
			std::cerr << "Rejection mismatch " << reason << std::endl;
			return 8;
		}
	}
	submit(waypointRequest);
	auto result = wait(waypointRequest.request_id(), true);
	if (result.stage() != MOVEMENT_OBSERVED || !result.has_displacement_m() || result.displacement_m() < MINIMUM_DISPLACEMENT_METERS) {
		return 9;
	}
	// Reconnect and retry identical ID: backend must return its retained status.
	stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto duplicate = submit(waypointRequest);
	if (duplicate.stage() != MOVEMENT_OBSERVED) {
		return 10;
	}
	WorldSnapshot finalView;
	if (!query(finalView)) {
		return 11;
	}
	Print("final-view", finalView);
	std::cout << "SMOKE_PASS request=" << waypointRequest.request_id() << " movement_m=" << result.displacement_m() << std::endl;
	return 0;
}
