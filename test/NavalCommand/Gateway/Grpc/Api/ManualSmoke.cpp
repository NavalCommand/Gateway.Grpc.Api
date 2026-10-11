#include <NavalCommand/Gateway/Grpc/Api/ManualSmoke.h>
#include <algorithm>
#include <chrono>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <initializer_list>
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
bool HasCoordinates(const navalcommand::gateway::v1::KinematicsObservation &kinematics) {
	return kinematics.has_exact_position() ||
		(kinematics.has_partial_position() && kinematics.partial_position().has_latitude_deg() &&
			kinematics.partial_position().has_longitude_deg());
}
double Latitude(const navalcommand::gateway::v1::KinematicsObservation &kinematics) {
	return kinematics.has_exact_position() ? kinematics.exact_position().latitude_deg() : kinematics.partial_position().latitude_deg();
}
double Longitude(const navalcommand::gateway::v1::KinematicsObservation &kinematics) {
	return kinematics.has_exact_position() ? kinematics.exact_position().longitude_deg() : kinematics.partial_position().longitude_deg();
}

template <class T> void Print(const char *label, const T &value) {
	std::string jsonText;
	google::protobuf::util::MessageToJsonString(value, &jsonText);
	std::cout << label << " " << jsonText << std::endl;
}
}

namespace NavalCommand::Gateway::Grpc::Api {
int ReloadSmoke(const std::string &endpoint) {
	auto stub = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto query = [&](ObserverSnapshot &snapshot) {
		Session session;
		grpc::ClientContext connectContext;
		connectContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->Connect(&connectContext, Query{}, &session).ok() || session.schema_version() != 4) {
			return false;
		}
		Query scopedQuery;
		scopedQuery.set_session_id(session.session_id());
		*scopedQuery.mutable_observer() = session.observer();
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		return stub->GetView(&context, scopedQuery, &snapshot).ok();
	};
	ObserverSnapshot snapshot;
	if (!query(snapshot)) {
		std::cerr << "Observer snapshot unavailable\n";
		return 3;
	}
	Print("observer-snapshot", snapshot);

	const auto originalSnapshot = snapshot;
	std::string originalObservationId;
	for (const auto &unit : originalSnapshot.units()) {
		Query discoveryQuery;
		discoveryQuery.set_session_id(originalSnapshot.session_id());
		*discoveryQuery.mutable_observer() = originalSnapshot.observer();
		*discoveryQuery.mutable_observation() = unit.identity();
		OrderDiscovery discovery;
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (stub->DiscoverOrders(&context, discoveryQuery, &discovery).ok() && HasCoordinates(unit.kinematics())) {
			for (const auto &eligibility : discovery.eligibility().items()) {
				if (eligibility.kind() == ORDER_KIND_IDLE_WAYPOINT) {
					originalObservationId = unit.identity().value();
					break;
				}
			}
		}
		if (!originalObservationId.empty()) {
			break;
		}
	}
	bool changed = false;
	for (int pollIndex = 0; pollIndex < MAXIMUM_STATUS_POLLS; ++pollIndex) {
		std::this_thread::sleep_for(POLL_INTERVAL);
		ObserverSnapshot nextSnapshot;
		if (query(nextSnapshot) && nextSnapshot.session_id() != originalSnapshot.session_id()) {
			snapshot = nextSnapshot;
			changed = true;
			break;
		}
	}
	const auto originalOwn = std::ranges::find_if(originalSnapshot.units(), [&](const auto &instance) {
		return !originalObservationId.empty() && instance.identity().value() == originalObservationId;
	});
	if (!changed || originalOwn == originalSnapshot.units().end()) {
		return 12;
	}

	OrderResult rejected;
	OrderRequest stale;
	stale.set_session_id(originalSnapshot.session_id());
	stale.set_request_id("reload-stale");
	*stale.mutable_observer() = originalSnapshot.observer();
	*stale.mutable_observation() = originalOwn->identity();
	stale.set_based_on_version(originalSnapshot.version());
	stale.mutable_waypoint()->set_latitude_deg(Latitude(originalOwn->kinematics()) + WAYPOINT_LATITUDE_OFFSET_DEGREES);
	stale.mutable_waypoint()->set_longitude_deg(Longitude(originalOwn->kinematics()));
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

	stale.set_session_id(snapshot.session_id());
	*stale.mutable_observer() = snapshot.observer();
	stale.set_request_id("reload-old-handle");
	stale.set_based_on_version(snapshot.version());
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

int RouteSmoke(const std::string &endpoint, bool blocked) {
	auto stub = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto query = [&](ObserverSnapshot &snapshot) {
		Session session;
		grpc::ClientContext connectContext;
		connectContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->Connect(&connectContext, Query{}, &session).ok() || session.schema_version() != 4) {
			return false;
		}
		Query scopedQuery;
		scopedQuery.set_session_id(session.session_id());
		*scopedQuery.mutable_observer() = session.observer();
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		return stub->GetView(&context, scopedQuery, &snapshot).ok();
	};
	ObserverSnapshot snapshot;
	if (!query(snapshot)) {
		std::cerr << "Observer snapshot unavailable\n";
		return 3;
	}
	Print("observer-snapshot", snapshot);
	if (snapshot.units().empty()) {
		return 4;
	}

	const UnitObservation *selectedUnit = nullptr;
	for (int attempt = 0; attempt < (blocked ? MAXIMUM_PROTECTED_STATE_POLLS : 1); ++attempt) {
		for (const auto &unit : snapshot.units()) {
			if (unit.identity().value().empty() || !HasCoordinates(unit.kinematics())) {
				continue;
			}
			Query candidateQuery;
			candidateQuery.set_session_id(snapshot.session_id());
			*candidateQuery.mutable_observer() = snapshot.observer();
			*candidateQuery.mutable_observation() = unit.identity();
			OrderDiscovery candidateDiscovery;
			grpc::ClientContext candidateContext;
			candidateContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			if (!stub->DiscoverOrders(&candidateContext, candidateQuery, &candidateDiscovery).ok()) {
				continue;
			}
			for (auto &eligibility : candidateDiscovery.eligibility().items()) {
				if (eligibility.availability() == (blocked ? BLOCKED : EXECUTABLE) && (!blocked || eligibility.reason() == "protected_native_action") &&
					eligibility.kind() == ORDER_KIND_IDLE_WAYPOINT) {
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
	discoveryQuery.set_session_id(snapshot.session_id());
	*discoveryQuery.mutable_observer() = snapshot.observer();
	*discoveryQuery.mutable_observation() = selectedUnit->identity();
	OrderDiscovery discovery;
	{
		grpc::ClientContext context;
		if (!stub->DiscoverOrders(&context, discoveryQuery, &discovery).ok()) {
			return 6;
		}
	}
	Print("discovery", discovery);

	OrderRequest waypointRequest;
	waypointRequest.set_session_id(snapshot.session_id());
	*waypointRequest.mutable_observer() = snapshot.observer();
	*waypointRequest.mutable_observation() = selectedUnit->identity();
	waypointRequest.set_request_id("route-smoke-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
	waypointRequest.set_based_on_version(snapshot.version());
	waypointRequest.mutable_waypoint()->set_latitude_deg(Latitude(selectedUnit->kinematics()) + WAYPOINT_LATITUDE_OFFSET_DEGREES);
	waypointRequest.mutable_waypoint()->set_longitude_deg(Longitude(selectedUnit->kinematics()));
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
			*statusQuery.mutable_observer() = waypointRequest.observer();
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
		ObserverSnapshot protectedView;
		if (!query(protectedView)) {
			return 7;
		}
		Print("protected-after", protectedView);
		return 0;
	}

	int invalidRequestIndex = 0;
	for (const auto *reason : {"stale_session", "wrong_side_or_invalid_entity", "invalid_arguments", "stale_observation"}) {
		auto invalidRequest = waypointRequest;
		invalidRequest.set_request_id(waypointRequest.request_id() + "-bad" + std::to_string(invalidRequestIndex++));
		std::string expectedReason = reason;
		if (expectedReason == "stale_session") {
			invalidRequest.set_session_id("old");
		}
		if (expectedReason == "wrong_side_or_invalid_entity") {
			invalidRequest.mutable_observation()->set_value("opponent-does-not-exist");
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
	stub = navalcommand::gateway::v1::Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto duplicate = submit(waypointRequest);
	if (duplicate.stage() != MOVEMENT_OBSERVED) {
		return 10;
	}
	ObserverSnapshot finalView;
	if (!query(finalView)) {
		return 11;
	}
	Print("final-view", finalView);
	std::cout << "SMOKE_PASS request=" << waypointRequest.request_id() << " movement_m=" << result.displacement_m() << std::endl;
	return 0;
}
}
