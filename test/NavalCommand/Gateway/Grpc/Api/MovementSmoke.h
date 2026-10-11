#pragma once
#include <chrono>
#include <cmath>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <stdexcept>
#include <initializer_list>
#include <thread>

// Explicitly requested development regression, using only the public RPCs.
inline int MovementSmoke(const std::string &endpoint) {
	using namespace navalcommand::gateway::v1;
	constexpr auto RPC_TIMEOUT = std::chrono::seconds(3);
	constexpr auto POLL_INTERVAL = std::chrono::milliseconds(250);
	constexpr int MAXIMUM_POLLS = 240;
	auto stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto print = [](const char *label, const auto &message) {
		std::string json;
		google::protobuf::util::MessageToJsonString(message, &json);
		std::cout << label << " " << json << std::endl;
	};
	auto view = [&]() {
		ObserverSnapshot snapshot;
		Session session;
		grpc::ClientContext connectContext;
		connectContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->Connect(&connectContext, Query{}, &session).ok() || session.schema_version() != 4) {
			throw std::runtime_error("session_unavailable");
		}
		Query query;
		query.set_session_id(session.session_id());
		*query.mutable_observer() = session.observer();
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->GetView(&context, query, &snapshot).ok()) {
			throw std::runtime_error("observer_snapshot_unavailable");
		}
		return snapshot;
	};
	auto initial = view();
	std::string entityId;
	for (const auto &unit : initial.units()) {
		if (unit.identity().value().empty() || !unit.kinematics().position().has_known()) {
			continue;
		}
		Query discoveryQuery;
		discoveryQuery.set_session_id(initial.session_id());
		*discoveryQuery.mutable_observer() = initial.observer();
		*discoveryQuery.mutable_observation() = unit.identity();
		OrderDiscovery discovery;
		grpc::ClientContext discoveryContext;
		discoveryContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->DiscoverOrders(&discoveryContext, discoveryQuery, &discovery).ok()) {
			continue;
		}
		print("route-discovery", discovery);
		for (const auto &eligibility : discovery.eligibility().items()) {
			if (eligibility.kind() == ORDER_KIND_REPLACE_ROUTE && eligibility.availability() == EXECUTABLE) {
				entityId = unit.identity().value();
				break;
			}
		}
		if (!entityId.empty()) {
			break;
		}
	}
	if (entityId.empty()) {
		print("unavailable-view", initial);
		return 30;
	}
	const auto prefix = "route-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
	auto request = [&](const std::string &suffix, RouteMode mode, double latitudeOffset, double longitudeOffset) {
		auto snapshot = view();
		OrderRequest command;
		command.set_session_id(snapshot.session_id());
		*command.mutable_observer() = snapshot.observer();
		command.mutable_observation()->set_value(entityId);
		command.set_request_id(prefix + suffix);
		command.set_based_on_version(snapshot.version());
		for (const auto &unit : snapshot.units()) {
			if (!unit.kinematics().position().has_known()) {
				continue;
			}
			if (unit.identity().value() == entityId) {
				command.mutable_waypoint()->set_latitude_deg(unit.kinematics().position().known().coordinates().latitude_deg() + latitudeOffset);
				command.mutable_waypoint()->set_longitude_deg(unit.kinematics().position().known().coordinates().longitude_deg() + longitudeOffset);
				command.mutable_waypoint()->set_route_mode(mode);
			}
		}
		return command;
	};
	auto submit = [&](const OrderRequest &command, bool movement = false) {
		print("request", command);
		OrderResult receipt;
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->SubmitOrder(&context, command, &receipt).ok()) {
			throw std::runtime_error("submit_failed");
		}
		for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
			if (receipt.stage() == REJECTED || receipt.stage() == EXECUTION_UNKNOWN || receipt.stage() == MOVEMENT_OBSERVED ||
				(!movement && (receipt.stage() == ROUTE_MUTATED || receipt.stage() == NATIVE_TASK_CREATED))) {
				break;
			}
			std::this_thread::sleep_for(POLL_INTERVAL);
			Query query;
			query.set_session_id(command.session_id());
			*query.mutable_observer() = command.observer();
			query.set_request_id(command.request_id());
			grpc::ClientContext statusContext;
			statusContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			if (!stub->GetStatus(&statusContext, query, &receipt).ok()) {
				throw std::runtime_error("status_failed");
			}
		}
		print("receipt", receipt);
		return receipt;
	};
	auto verify = [&](const OrderResult &receipt, std::initializer_list<std::string> expectedLabels) {
		if (receipt.stage() != ROUTE_MUTATED && receipt.stage() != MOVEMENT_OBSERVED) {
			throw std::runtime_error("route_mutation_not_verified");
		}
		if (receipt.route_after_size() != expectedLabels.size()) {
			throw std::runtime_error("route_size_mismatch");
		}
		int index = 0;
		for (const auto &label : expectedLabels) {
			if (receipt.route_after(index++).label() != "NC-" + prefix + label) {
				throw std::runtime_error("route_sequence_mismatch");
			}
		}
		bool observed = false;
		for (int poll = 0; poll < 20 && !observed; ++poll) {
			auto snapshot = view();
			for (const auto &unit : snapshot.units()) {
				if (unit.identity().value() == entityId && unit.has_navigation() &&
					unit.navigation().planned_route().has_known() &&
					unit.navigation().planned_route().known().points_size() == receipt.route_after_size()) {
					observed = true;
					for (int point = 0; point < receipt.route_after_size(); ++point) {
						const auto &observedPoint = unit.navigation().planned_route().known().points(point);
						const auto &receiptPoint = receipt.route_after(point);
						observed = observed && observedPoint.label() == receiptPoint.label() &&
							observedPoint.latitude_deg() == receiptPoint.latitude_deg() &&
							observedPoint.longitude_deg() == receiptPoint.longitude_deg();
					}
				}
			}
			if (observed) {
				print("observed-route", snapshot);
			} else {
				std::this_thread::sleep_for(POLL_INTERVAL);
			}
		}
		if (!observed) {
			throw std::runtime_error("route_not_observed");
		}
	};
	try {
		// First establish motion, then redirect and extend the moving vessel.
		auto first = request("A", IDLE_WAYPOINT, 0.03, 0);
		if (submit(first, true).stage() != MOVEMENT_OBSERVED) {
			throw std::runtime_error("idle_waypoint_first_movement_failed");
		}
		print("idle-waypoint-moving-view", view());
		auto replaced = request("B", REPLACE_ROUTE, 0, 0.03);
		verify(submit(replaced), {"B"});
		verify(submit(request("C", APPEND_WAYPOINT, 0.03, 0.03)), {"B", "C"});
		verify(submit(request("D", APPEND_WAYPOINT, -0.03, 0.03)), {"B", "C", "D"});
		auto final = request("E", REPLACE_ROUTE, -0.03, 0);
		auto replacedMultiple = submit(final, true);
		if (replacedMultiple.stage() != MOVEMENT_OBSERVED) {
			throw std::runtime_error("replacement_movement_not_observed");
		}
		verify(replacedMultiple, {"E"});
		if (replacedMultiple.route_before_size() != 3) {
			throw std::runtime_error("multi_route_before_missing");
		}
		stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
		verify(submit(final), {"E"});
		for (const auto *reason : {"stale_session", "wrong_side_or_invalid_entity", "invalid_arguments", "stale_observation", "unsupported_route_mode"}) {
			auto invalid = request(reason, REPLACE_ROUTE, 0.03, 0);
			const std::string expected = reason;
			if (expected == "stale_session") {
				invalid.set_session_id("old");
			}
			if (expected == "wrong_side_or_invalid_entity") {
				invalid.mutable_observation()->set_value("unauthorized");
			}
			if (expected == "invalid_arguments") {
				invalid.mutable_waypoint()->set_latitude_deg(100);
			}
			if (expected == "stale_observation") {
				invalid.set_based_on_version(0);
			}
			if (expected == "unsupported_route_mode") {
				invalid.mutable_waypoint()->set_route_mode(static_cast<RouteMode>(99));
			}
			auto rejected = submit(invalid);
			if (rejected.stage() != REJECTED || rejected.reason() != expected) {
				throw std::runtime_error("negative_request_mismatch");
			}
		}
		verify(submit(final), {"E"});
		std::cout << "MOVEMENT_ROUTE_PASS" << std::endl;
		return 0;
	} catch (const std::exception &exception) {
		std::cerr << exception.what() << std::endl;
		return 31;
	}
}
