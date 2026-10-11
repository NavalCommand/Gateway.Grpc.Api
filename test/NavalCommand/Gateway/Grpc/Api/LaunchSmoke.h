#pragma once
#include <algorithm>
#include <chrono>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <stdexcept>
#include <thread>

inline int LaunchSmoke(const std::string &endpoint) {
	using namespace navalcommand::gateway::v1;
	constexpr auto RPC_TIMEOUT = std::chrono::seconds(3);
	constexpr auto POLL_INTERVAL = std::chrono::milliseconds(500);
	constexpr int MAXIMUM_POLLS = 2400;
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
			throw std::runtime_error("launch_session_unavailable");
		}
		Query query;
		query.set_session_id(session.session_id());
		*query.mutable_observer() = session.observer();
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->GetView(&context, query, &snapshot).ok()) {
			throw std::runtime_error("launch_observer_snapshot_unavailable");
		}
		return snapshot;
	};
	try {
		auto initial = view();
		print("launch-initial-observer-snapshot", initial);
		OrderRequest request;
		request.set_session_id(initial.session_id());
		*request.mutable_observer() = initial.observer();
		request.set_request_id("launch-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
		request.set_based_on_version(initial.version());
		bool found = false;
		bool resourceBlocked = false;
		OrderDiscovery discovery;
		for (const auto &unit : initial.units()) {
			if (!unit.has_air_operations()) {
				continue;
			}
			Query discoveryQuery;
			discoveryQuery.set_session_id(initial.session_id());
			*discoveryQuery.mutable_observer() = initial.observer();
			*discoveryQuery.mutable_observation() = unit.identity();
			grpc::ClientContext discoveryContext;
			discoveryContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			if (!stub->DiscoverOrders(&discoveryContext, discoveryQuery, &discovery).ok()) {
				continue;
			}
			print("launch-discovery", discovery);
			for (const auto &eligibility : discovery.eligibility().items()) {
				if (eligibility.kind() != ORDER_KIND_LAUNCH_AIRCRAFT || !eligibility.has_launch_option() ||
					(eligibility.availability() != EXECUTABLE && eligibility.reason() != "resources_unavailable")) {
					continue;
				}
				for (const auto &entry : unit.air_operations().inventory().items()) {
					const bool loadoutObserved = std::ranges::any_of(entry.loadouts().items(), [&](const auto &loadout) {
						return loadout.value() == eligibility.launch_option().selection().loadout().value();
					});
					if (entry.type().has_known() &&
						entry.type().known().value() == eligibility.launch_option().selection().type().value() &&
						entry.squadron().has_known() &&
						entry.squadron().known() == eligibility.launch_option().selection().squadron() &&
						loadoutObserved && entry.unreserved_count().has_known() && entry.unreserved_count().known() > 0) {
						*request.mutable_launch_aircraft() = eligibility.launch_option();
						found = true;
						resourceBlocked = eligibility.availability() != EXECUTABLE;
						break;
					}
				}
				if (found) {
					break;
				}
			}
			if (found) {
				break;
			}
		}
		if (!found) {
			throw std::runtime_error("launch_option_unavailable");
		}
		auto send = [&](const OrderRequest &command) {
			print("launch-request", command);
			OrderResult receipt;
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			if (!stub->SubmitOrder(&context, command, &receipt).ok()) {
				throw std::runtime_error("launch_submit_failed");
			}
			for (int poll = 0; poll < 40 && receipt.stage() == REQUEST_RECEIVED; ++poll) {
				std::this_thread::sleep_for(POLL_INTERVAL);
				Query query;
				query.set_session_id(command.session_id());
				*query.mutable_observer() = command.observer();
				grpc::ClientContext statusContext;
				statusContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
				if (!stub->GetStatus(&statusContext, query, &receipt).ok()) {
					throw std::runtime_error("launch_status_failed");
				}
			}
			print("launch-receipt", receipt);
			return receipt;
		};
		if (resourceBlocked) {
			auto shortage = request;
			shortage.set_request_id(request.request_id() + "-resources");
			auto rejected = send(shortage);
			if (rejected.stage() != REJECTED || rejected.reason() != "resources_unavailable") {
				throw std::runtime_error("resource_shortage_rejection_missing");
			}
			bool restored = false;
			for (int poll = 0; poll < 40 && !restored; ++poll) {
				auto snapshot = view();
				Query discoveryQuery;
				discoveryQuery.set_session_id(snapshot.session_id());
				*discoveryQuery.mutable_observer() = snapshot.observer();
				*discoveryQuery.mutable_observation() = request.observation();
				grpc::ClientContext discoveryContext;
				discoveryContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
				if (stub->DiscoverOrders(&discoveryContext, discoveryQuery, &discovery).ok()) {
					for (const auto &eligibility : discovery.eligibility().items()) {
						restored = restored || (eligibility.kind() == ORDER_KIND_LAUNCH_AIRCRAFT && eligibility.has_launch_option() &&
							eligibility.launch_option().selection().type().value() == request.launch_aircraft().selection().type().value() &&
							eligibility.launch_option().selection().squadron() == request.launch_aircraft().selection().squadron() &&
							eligibility.launch_option().selection().loadout().value() == request.launch_aircraft().selection().loadout().value() &&
							eligibility.availability() == EXECUTABLE);
					}
				}
				if (!restored) {
					std::this_thread::sleep_for(POLL_INTERVAL);
				}
			}
			if (!restored) {
				throw std::runtime_error("resource_shortage_not_restored");
			}
			request.set_based_on_version(view().version());
		}
		for (const auto *reason : {"invalid_arguments", "unsupported_aircraft", "invalid_squadron", "invalid_loadout", "wrong_side_or_invalid_entity", "stale_observation"}) {
			auto invalid = request;
			invalid.set_request_id(request.request_id() + reason);
			const std::string expected = reason;
			if (expected == "invalid_arguments") {
				invalid.mutable_launch_aircraft()->set_count(2);
			}
			if (expected == "unsupported_aircraft") {
				invalid.mutable_launch_aircraft()->mutable_selection()->mutable_type()->set_value("absent-class");
			}
			if (expected == "invalid_squadron") {
				invalid.mutable_launch_aircraft()->mutable_selection()->set_squadron("absent-squadron");
			}
			if (expected == "invalid_loadout") {
				invalid.mutable_launch_aircraft()->mutable_selection()->mutable_loadout()->set_value("absent-loadout");
			}
			if (expected == "wrong_side_or_invalid_entity") {
				invalid.mutable_observation()->set_value("not-own");
			}
			if (expected == "stale_observation") {
				invalid.set_based_on_version(0);
			}
			auto receipt = send(invalid);
			if (receipt.stage() != REJECTED || receipt.reason() != expected) {
				throw std::runtime_error("launch_negative_mismatch");
			}
		}
		request.set_based_on_version(view().version());
		auto accepted = send(request);
		if (accepted.stage() != NATIVE_TASK_CREATED && accepted.stage() != PREPARING) {
			throw std::runtime_error("launch_not_accepted");
		}
		auto duplicate = send(request);
		if (duplicate.stage() != accepted.stage()) {
			throw std::runtime_error("launch_duplicate_mismatch");
		}
		Stage previous = accepted.stage();
		bool spawnedObserved = false;
		for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
			std::this_thread::sleep_for(POLL_INTERVAL);
			Query query;
			query.set_session_id(request.session_id());
			*query.mutable_observer() = request.observer();
			query.set_request_id(request.request_id());
			OrderResult receipt;
			grpc::ClientContext context;
			context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
			if (!stub->GetStatus(&context, query, &receipt).ok()) {
				throw std::runtime_error("launch_status_failed");
			}
			if (receipt.stage() != previous) {
				print("launch-progress", receipt);
				previous = receipt.stage();
			}
			if (receipt.stage() == AIRCRAFT_SPAWNED || receipt.stage() == AIRBORNE) {
				auto snapshot = view();
				for (const auto &unit : snapshot.units()) {
					if (unit.identity().value().empty() || !unit.kinematics().position().has_known()) {
						continue;
					}
					if (receipt.has_spawned_observation_id() && unit.identity().value() == receipt.spawned_observation_id().value()) {
						if (receipt.stage() == AIRCRAFT_SPAWNED && unit.has_aircraft() && unit.aircraft().airborne().has_known() && !unit.aircraft().airborne().known()) {
							spawnedObserved = true;
							if (poll % 20 == 0) {
								print("grounded-observed-contact", unit);
							}
						}
						if (receipt.stage() == AIRBORNE && unit.has_aircraft() && unit.aircraft().airborne().has_known() && unit.aircraft().airborne().known() && spawnedObserved) {
							print("airborne-observed-contact", unit);
							std::cout << "FACILITY_LAUNCH_PASS" << std::endl;
							return 0;
						}
					}
				}
			}
			if (receipt.stage() == REJECTED || receipt.stage() == EXECUTION_UNKNOWN) {
				throw std::runtime_error("launch_execution_unknown_or_rejected");
			}
		}
		throw std::runtime_error("launch_timeout");
	} catch (const std::exception &exception) {
		std::cerr << exception.what() << std::endl;
		return 40;
	}
}
