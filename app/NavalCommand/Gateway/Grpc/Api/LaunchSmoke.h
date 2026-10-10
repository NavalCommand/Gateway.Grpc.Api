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
		for (const auto &facility : initial.facilities()) {
			for (const auto &stock : facility.stocks()) {
				for (const auto &option : stock.launch_options()) {
					if (stock.unreserved_count() > 0 && (option.availability() == EXECUTABLE || option.reason() == "resources_unavailable")) {
						request.set_entity_id(facility.entity_id());
						auto launch = request.mutable_facility_launch();
						launch->set_class_key(stock.class_key());
						launch->set_squadron(stock.squadron());
						launch->set_loadout(option.loadout());
						launch->set_count(1);
						found = true;
						resourceBlocked = option.availability() != EXECUTABLE;
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
				for (const auto &facility : snapshot.facilities()) {
					if (facility.entity_id() == request.entity_id()) {
						for (const auto &stock : facility.stocks()) {
							if (stock.class_key() == request.facility_launch().class_key() && stock.squadron() == request.facility_launch().squadron()) {
								for (const auto &option : stock.launch_options()) {
									restored = restored || (option.loadout() == request.facility_launch().loadout() && option.availability() == EXECUTABLE);
								}
							}
						}
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
				invalid.mutable_facility_launch()->set_count(2);
			}
			if (expected == "unsupported_aircraft") {
				invalid.mutable_facility_launch()->set_class_key("absent-class");
			}
			if (expected == "invalid_squadron") {
				invalid.mutable_facility_launch()->set_squadron("absent-squadron");
			}
			if (expected == "invalid_loadout") {
				invalid.mutable_facility_launch()->set_loadout("absent-loadout");
			}
			if (expected == "wrong_side_or_invalid_entity") {
				invalid.set_entity_id("not-own");
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
				for (const auto &unit : snapshot.own_units()) {
					if (unit.id() == receipt.spawned_entity_id()) {
						if (receipt.stage() == AIRCRAFT_SPAWNED && unit.has_airborne() && !unit.airborne()) {
							spawnedObserved = true;
							if (poll % 20 == 0) {
								print("grounded-regular-unit", unit);
							}
						}
						if (receipt.stage() == AIRBORNE && unit.has_airborne() && unit.airborne() && spawnedObserved) {
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
