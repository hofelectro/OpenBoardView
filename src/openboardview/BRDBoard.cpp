#include "BRDBoard.h"

#include "FileFormats/BRDFile.h"

#include <cerrno>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace std::placeholders;

const string BRDBoard::kNetUnconnectedPrefix = "UNCONNECTED";
const string BRDBoard::kComponentDummyName   = "...";

BRDBoard::BRDBoard(const BRDFile *const boardFile)
    : m_file(boardFile) {
	// TODO: strip / trim all strings, especially those used as keys
	// TODO: just loop through original arrays?
	vector<BRDPart> m_parts(m_file->num_parts);
	vector<BRDPin> m_pins(m_file->num_pins);
	vector<BRDNail> m_nails(m_file->num_nails);
	vector<BRDPoint> m_points(m_file->num_format);

	m_parts  = m_file->parts;
	m_pins   = m_file->pins;
	m_nails  = m_file->nails;
	m_points = m_file->format;

	// Set outline
	{
		for (auto &brdPoint : m_points) {
			outline_.push_back({brdPoint.x, brdPoint.y});
		}
	}

	// Populate map of unique nets
	std::map<string, Net> net_map;
	{
		// adding special net 'UNCONNECTED'
		Net net_nc;
		net_nc.name          = kNetUnconnectedPrefix;
		net_nc.is_ground     = false;
		net_map[net_nc.name] = net_nc;

		// handle all the others
		for (auto &brd_nail : m_nails) {
			Net net;

			// copy NET name and number (probe)
			net.name = string(brd_nail.net);

			// avoid having multiple UNCONNECTED<XXX> references
			if (is_prefix(kNetUnconnectedPrefix, net.name)) continue;

			// check whether the pin represents ground
			net.is_ground = (net.name == "GND");
			net.number    = brd_nail.probe;

			if (brd_nail.side == 1) {
				net.board_side = kBoardSideTop;
			} else {
				net.board_side = kBoardSideBottom;
			}

			// so we can find nets later by name (making unique by name)
			net_map[net.name] = net;
		}
	}

	// Populate parts
	{
		for (auto &brd_part : m_parts) {
			Component comp;

			comp.name    = string(brd_part.name);
			comp.mfgcode = std::move(brd_part.mfgcode);

			comp.p1 = {brd_part.p1.x, brd_part.p1.y};
			comp.p2 = {brd_part.p2.x, brd_part.p2.y};

			// is it some dummy component to indicate test pads?
			if (is_prefix(kComponentDummyName, comp.name)) comp.component_type = Component::kComponentTypeDummy;

			// check what side the board is on (sorcery?)
			if (brd_part.mounting_side == BRDPartMountingSide::Top) {
				comp.board_side = kBoardSideTop;
			} else if (brd_part.mounting_side == BRDPartMountingSide::Bottom) {
				comp.board_side = kBoardSideBottom;
			} else {
				comp.board_side = kBoardSideBoth;
			}

			comp.mount_type = (brd_part.part_type == BRDPartType::SMD) ? Component::kMountTypeSMD : Component::kMountTypeDIP;

			components_.push_back(comp);
		}
	}

	// Populate pins
	{
		// generate dummy component as reference
		Component comp_dummy;
		comp_dummy.name           = kComponentDummyName;
		comp_dummy.component_type = Component::kComponentTypeDummy;

		// NOTE: originally the pin diameter depended on part.name[0] == 'U' ?
		unsigned int pin_idx  = 0;
		unsigned int part_idx = 1;
		auto pins             = m_pins;
		auto parts            = m_parts;

		for (size_t i = 0; i < pins.size(); i++) {
			// (originally from BoardView::DrawPins)
			const BRDPin &brd_pin = pins[i];
			auto type = Pin::kPinTypeComponent;
			auto comp = std::ref(components_[brd_pin.part - 1]);
			auto net = std::ref(net_map[kNetUnconnectedPrefix]);

			if (comp.get().is_dummy()) {
				// component is virtual, i.e. "...", pin is test pad
				type = Pin::kPinTypeTestPad;
				comp = std::ref(comp_dummy);
			}

			// set net reference (here's our NET key string again)
			string net_name = string(brd_pin.net);
			if (net_map.count(net_name)) {
				// there is a net with that name in our map
				net = net_map[net_name];

			} else {
				// no net with that name registered, so create one
				if (!net_name.empty()) {
					if (is_prefix(kNetUnconnectedPrefix, net_name)) {
						// pin is unconnected, so reference our special net
						net  = std::ref(net_map[kNetUnconnectedPrefix]);
						type = Pin::kPinTypeNotConnected;
					} else {
						// indeed a new net
						Net net;
						net.name       = net_name;
						net.board_side = comp.get().board_side;
						// NOTE: net->number not set
						net_map[net_name] = net;
						net          = std::ref(net_map[net_name]);
					}
				} else {
					// not sure this can happen -> no info
					// It does happen in .fz apparently and produces a SEGFAULT… Use
					// unconnected net.
					net  = std::ref(net_map[kNetUnconnectedPrefix]);
					type = Pin::kPinTypeNotConnected;
				}
			}

			Pin pin(net, comp);

			pin.type = type;
			if (pin.type == Pin::kPinTypeTestPad) {
				pin.board_side = pin.net->board_side;
			} else {
				pin.board_side = pin.component->board_side;
			}

			// determine pin number on part
			++pin_idx;
			if (brd_pin.part != part_idx) {
				part_idx = brd_pin.part;
				pin_idx  = 1;
			}
			if (brd_pin.snum)
				pin.number = brd_pin.snum;
			else
				pin.number = std::to_string(pin_idx);

			// copy position
			pin.position = Point(brd_pin.pos.x, brd_pin.pos.y);

			// TODO: should either depend on file specs or type etc
			//
			//  if(brd_pin.radius) pin->diameter = brd_pin.radius; // some format
			//  (.fz) contains a radius field
			//    else pin->diameter = 0.5f;
			pin.diameter = brd_pin.radius; // some format (.fz) contains a radius field

			pin.net->pins.push_back(pin);
			pin.component->pins.push_back(pin);
			pins_.push_back(pin);
		}

		// remove all dummy components from vector, add our official dummy
		components_.erase(
		    remove_if(begin(components_), end(components_), [](Component &comp) { return comp.is_dummy(); }),
		    end(components_));

		components_.push_back(comp_dummy);
	}

	// Populate Net vector by using the map. (sorted by keys)
	for (auto &net : net_map) {
		nets_.push_back(std::move(net.second));
	}

	// Sort components by name
	sort(begin(components_), end(components_), [](const Component &lhs, const Component &rhs) {
		return lhs.name < rhs.name;
	});
}

BRDBoard::~BRDBoard() {}

std::vector<Component> &BRDBoard::Components() {
	return components_;
}

std::vector<Pin> &BRDBoard::Pins() {
	return pins_;
}

std::vector<Net> &BRDBoard::Nets() {
	return nets_;
}

std::vector<Point> &BRDBoard::OutlinePoints() {
	return outline_;
}

Board::EBoardType BRDBoard::BoardType() {
	return kBoardTypeBRD;
}
