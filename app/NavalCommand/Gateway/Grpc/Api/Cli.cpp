#include <chrono>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <navalcommand/gateway/v1/service.grpc.pb.h>
#include <string>
#include <thread>
using namespace navalcommand::gateway::v1;
template <class T> void print(const char *label, const T &value) {
	std::string s;
	google::protobuf::util::MessageToJsonString(value, &s);
	std::cout << label << " " << s << std::endl;
}
int main(int argc, char **argv) {
	std::string endpoint = "127.0.0.1:50051";
	bool smoke = false, blocked = false, reload = false;
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--schema") {
			std::cout << WorldSnapshot::descriptor()->full_name() << " " << Gateway::service_full_name() << std::endl;
			return 0;
		}
		if (a == "--endpoint" && i + 1 < argc) {
			endpoint = argv[++i];
		} else if (a == "--smoke") {
			smoke = true;
		} else if (a == "--blocked") {
			blocked = true;
		} else if (a == "--reload") {
			reload = true;
		} else if (a != "--endpoint") {
			std::cerr << "Usage: naval-cli [--endpoint 127.0.0.1:port] "
						 "[--smoke|--blocked] (default read-only)\n";
			return 2;
		}
	}
	if (!endpoint.starts_with("127.0.0.1:")) {
		return 2;
	}
	auto stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto query = [&](WorldSnapshot &w) {
		grpc::ClientContext c;
		c.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
		return stub->GetView(&c, Query{}, &w).ok();
	};
	WorldSnapshot w;
	if (!query(w)) {
		std::cerr << "View unavailable\n";
		return 3;
	}
	print("view", w);
	if (reload) {
		const auto old = w;
		bool changed = false;
		for (int i = 0; i < 240; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			WorldSnapshot next;
			if (query(next) && next.session().session_id() != old.session().session_id()) {
				w = next;
				changed = true;
				break;
			}
		}
		if (!changed || old.own_units().empty() || w.own_units().empty()) {
			return 12;
		}
		OrderRequest stale;
		stale.set_session_id(old.session().session_id());
		stale.set_request_id("reload-stale");
		stale.set_entity_id(old.own_units(0).id());
		stale.set_based_on_version(old.observation_version());
		stale.mutable_waypoint()->set_latitude_deg(old.own_units(0).position().latitude_deg() + .03);
		stale.mutable_waypoint()->set_longitude_deg(old.own_units(0).position().longitude_deg());
		OrderResult rejected;
		{
			grpc::ClientContext c;
			if (!stub->SubmitOrder(&c, stale, &rejected).ok()) {
				return 13;
			}
		}
		print("old-session-rejected", rejected);
		if (rejected.stage() != REJECTED || rejected.reason() != "stale_session") {
			return 14;
		}
		stale.set_session_id(w.session().session_id());
		stale.set_request_id("reload-old-handle");
		stale.set_based_on_version(w.observation_version());
		{
			grpc::ClientContext c;
			if (!stub->SubmitOrder(&c, stale, &rejected).ok()) {
				return 15;
			}
		}
		print("old-handle-rejected", rejected);
		if (rejected.reason() != "wrong_side_or_invalid_entity") {
			return 16;
		}
		print("reloaded-view", w);
		std::cout << "RELOAD_PASS" << std::endl;
		return 0;
	}
	if (!smoke && !blocked) {
		return 0;
	}
	if (w.own_units().empty()) {
		return 4;
	}
	const Unit *u = nullptr;
	for (int attempt = 0; attempt < (blocked ? 100 : 1); ++attempt) {
		for (auto &x : w.own_units()) {
			for (auto &e : x.orders()) {
				if (e.availability() == (blocked ? BLOCKED : EXECUTABLE) && (!blocked || e.reason() == "protected_native_action") && e.kind() == "set_waypoint") {
					u = &x;
					break;
				}
			}
		}
		if (u) {
			break;
		}
		if (blocked) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			if (!query(w)) {
				break;
			}
		}
	}
	if (!u) {
		std::cerr << "Requested eligible example unavailable\n";
		return 5;
	}
	Query d;
	d.set_session_id(w.session().session_id());
	d.set_entity_id(u->id());
	OrderDiscovery discovery;
	{
		grpc::ClientContext c;
		if (!stub->DiscoverOrders(&c, d, &discovery).ok()) {
			return 6;
		}
	}
	print("discovery", discovery);
	OrderRequest r;
	r.set_session_id(w.session().session_id());
	r.set_entity_id(u->id());
	r.set_based_on_version(w.observation_version());
	r.set_request_id("smoke-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
	r.mutable_waypoint()->set_latitude_deg(u->position().latitude_deg() + 0.03);
	r.mutable_waypoint()->set_longitude_deg(u->position().longitude_deg());
	auto submit = [&](const OrderRequest &request) {
		grpc::ClientContext c;
		c.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
		OrderResult result;
		if (!stub->SubmitOrder(&c, request, &result).ok()) {
			result.set_reason("rpc_failed");
		}
		print("submit", result);
		return result;
	};
	auto wait = [&](const std::string &id, bool outcome) {
		OrderResult result;
		for (int i = 0; i < 240; ++i) {
			Query q;
			q.set_session_id(r.session_id());
			q.set_request_id(id);
			grpc::ClientContext c;
			c.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
			auto st = stub->GetStatus(&c, q, &result);
			if (!st.ok()) {
				break;
			}
			if (result.stage() == REJECTED || result.stage() == MOVEMENT_OBSERVED || (!outcome && result.stage() == NATIVE_TASK_CREATED)) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		print("status", result);
		return result;
	};
	if (blocked) {
		submit(r);
		return wait(r.request_id(), false).stage() == REJECTED ? 0 : 7;
	}
	int n = 0;
	for (auto reason : {"stale_session", "wrong_side_or_invalid_entity", "invalid_arguments", "stale_observation"}) {
		auto bad = r;
		bad.set_request_id(r.request_id() + "-bad" + std::to_string(n++));
		std::string v = reason;
		if (v == "stale_session") {
			bad.set_session_id("old");
		}
		if (v == "wrong_side_or_invalid_entity") {
			bad.set_entity_id("opponent-does-not-exist");
		}
		if (v == "invalid_arguments") {
			bad.mutable_waypoint()->set_latitude_deg(100);
		}
		if (v == "stale_observation") {
			bad.set_based_on_version(0);
		}
		auto result = submit(bad);
		if (result.stage() != REJECTED) {
			result = wait(bad.request_id(), false);
		}
		if (result.stage() != REJECTED || result.reason() != reason) {
			std::cerr << "Rejection mismatch " << reason << std::endl;
			return 8;
		}
	}
	submit(r);
	auto result = wait(r.request_id(), true);
	if (result.stage() != MOVEMENT_OBSERVED || !result.has_displacement_m() || result.displacement_m() < 5) {
		return 9;
	}
	// Reconnect and retry identical ID: backend must return its retained status.
	stub = Gateway::NewStub(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
	auto duplicate = submit(r);
	if (duplicate.stage() != MOVEMENT_OBSERVED) {
		return 10;
	}
	WorldSnapshot finalView;
	if (!query(finalView)) {
		return 11;
	}
	print("final-view", finalView);
	std::cout << "SMOKE_PASS request=" << r.request_id() << " movement_m=" << result.displacement_m() << std::endl;
	return 0;
}
