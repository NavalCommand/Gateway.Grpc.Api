#pragma once
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
		WorldSnapshot snapshot;
		grpc::ClientContext context;
		context.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
		if (!stub->GetView(&context, {}, &snapshot).ok()) {
			throw std::runtime_error("launch_view_unavailable");
		}
		return snapshot;
	};
	try {
		auto initial = view();
		print("launch-initial-view", initial);
		OrderRequest request;
		request.set_session_id(initial.session().session_id());
		request.set_request_id("launch-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
		request.set_based_on_version(initial.observation_version());
		bool found = false;
		bool resourceBlocked = false;
		for (const auto &unit : initial.instances()) {
			if (!unit.has_air_operations()) {
				continue;
			}
			Query discoveryQuery;
			discoveryQuery.set_session_id(initial.session().session_id());
			*discoveryQuery.mutable_unit_id() = unit.unit_id();
			OrderDiscovery discovery;
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
					if (entry.unit_type_id().value() == eligibility.launch_option().unit_type_id().value() &&
						entry.squadron() == eligibility.launch_option().squadron() &&
						entry.unreserved_count().has_known() && entry.unreserved_count().known() > 0) {
						*request.mutable_unit_id() = unit.unit_id();
						*request.mutable_aircraft_launch() = eligibility.launch_option();
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
				query.set_request_id(command.request_id());
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
				discoveryQuery.set_session_id(snapshot.session().session_id());
				*discoveryQuery.mutable_unit_id() = request.unit_id();
				OrderDiscovery discovery;
				grpc::ClientContext discoveryContext;
				discoveryContext.set_deadline(std::chrono::system_clock::now() + RPC_TIMEOUT);
				if (stub->DiscoverOrders(&discoveryContext, discoveryQuery, &discovery).ok()) {
					for (const auto &eligibility : discovery.eligibility().items()) {
						restored = restored || (eligibility.kind() == ORDER_KIND_LAUNCH_AIRCRAFT && eligibility.has_launch_option() &&
							eligibility.launch_option().unit_type_id().value() == request.aircraft_launch().unit_type_id().value() &&
							eligibility.launch_option().squadron() == request.aircraft_launch().squadron() &&
							eligibility.launch_option().loadout_id().value() == request.aircraft_launch().loadout_id().value() &&
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
			request.set_based_on_version(view().observation_version());
		}
		for (const auto *reason : {"invalid_arguments", "unsupported_aircraft", "invalid_squadron", "invalid_loadout", "wrong_side_or_invalid_entity", "stale_observation"}) {
			auto invalid = request;
			invalid.set_request_id(request.request_id() + reason);
			const std::string expected = reason;
			if (expected == "invalid_arguments") {
				invalid.mutable_aircraft_launch()->set_count(2);
			}
			if (expected == "unsupported_aircraft") {
				invalid.mutable_aircraft_launch()->mutable_unit_type_id()->set_value("absent-class");
			}
			if (expected == "invalid_squadron") {
				invalid.mutable_aircraft_launch()->set_squadron("absent-squadron");
			}
			if (expected == "invalid_loadout") {
				invalid.mutable_aircraft_launch()->mutable_loadout_id()->set_value("absent-loadout");
			}
			if (expected == "wrong_side_or_invalid_entity") {
				invalid.mutable_unit_id()->set_value("not-own");
			}
			if (expected == "stale_observation") {
				invalid.set_based_on_version(0);
			}
			auto receipt = send(invalid);
			if (receipt.stage() != REJECTED || receipt.reason() != expected) {
				throw std::runtime_error("launch_negative_mismatch");
			}
		}
		request.set_based_on_version(view().observation_version());
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
				for (const auto &unit : snapshot.instances()) {
					if (unit.unit_id().value().empty() || !unit.kinematics().position().has_known()) {
						continue;
					}
					if (unit.unit_id().value() == receipt.spawned_unit_id().value()) {
						if (receipt.stage() == AIRCRAFT_SPAWNED && unit.has_aircraft() && unit.aircraft().airborne().has_known() && !unit.aircraft().airborne().known()) {
							spawnedObserved = true;
							if (poll % 20 == 0) {
								print("grounded-regular-unit", unit);
							}
						}
						if (receipt.stage() == AIRBORNE && unit.has_aircraft() && unit.aircraft().airborne().has_known() && unit.aircraft().airborne().known() && spawnedObserved) {
							print("airborne-regular-unit", unit);
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
