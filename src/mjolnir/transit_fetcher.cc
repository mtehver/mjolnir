#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <unordered_set>
#include <thread>
#include <future>
#include <random>
#include <queue>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/format.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp>
#include <curl/curl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>

#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/graphtile.h>
#include <valhalla/baldr/datetime.h>

#include "proto/transit.pb.h"

using namespace boost::property_tree;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;

struct logged_error_t: public std::runtime_error {
  logged_error_t(const std::string& msg):std::runtime_error(msg) {
    LOG_ERROR(msg);
  }
};

struct curler_t {
  curler_t():connection(curl_easy_init(), [](CURL* c){curl_easy_cleanup(c);}),
    generator(std::chrono::system_clock::now().time_since_epoch().count()),
    distribution(static_cast<size_t>(500), static_cast<size_t>(1000)) {
    if(connection.get() == nullptr)
      throw logged_error_t("Failed to created CURL connection");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_ERRORBUFFER, error), "Failed to set error buffer");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_FOLLOWLOCATION, 1L), "Failed to set redirect option ");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_WRITEDATA, &result), "Failed to set write data ");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_WRITEFUNCTION, write_callback), "Failed to set writer ");
  }
  //for now we only need to handle json
  //with templates we could return a string or whatever
  ptree operator()(const std::string& url, const std::string& retry_if_no = "", bool gzip = true, boost::optional<size_t> timeout = boost::none) {
    //content encoding header
    if(gzip) {
      char encoding[] = "gzip"; //TODO: allow "identity" and "deflate"
      assert_curl(curl_easy_setopt(connection.get(), CURLOPT_ACCEPT_ENCODING, encoding), "Failed to set gzip content header ");
    }
    //set the url
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_URL, url.c_str()), "Failed to set URL ");
    //dont stop until we have something useful!
    ptree pt;
    while(true) {
      result.str("");
      long http_code = 0;
      std::string log_extra = "Couldn't fetch url ";
      //can we fetch this url
      LOG_DEBUG(url);
      if(curl_easy_perform(connection.get()) == CURLE_OK) {
        curl_easy_getinfo(connection.get(), CURLINFO_RESPONSE_CODE, &http_code);
        log_extra = std::to_string(http_code) + "'d ";
        //it should be 200 OK
        if(http_code == 200) {
          bool threw = false;
          try { read_json(result, pt); } catch (...) { threw = true; }
          //has to parse and have required info
          if(!threw && (retry_if_no.empty() || pt.get_child_optional(retry_if_no)))
            break;
          log_extra = "Unusable response ";
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout ? *timeout : distribution(generator)));
      LOG_WARN(log_extra + "retrying " + url);
    };
    return pt;
  }
  std::string last() const {
    return result.str();
  }
protected:
  void assert_curl(CURLcode code, const std::string& msg){
    if(code != CURLE_OK)
      throw logged_error_t(msg + error);
  };
  static size_t write_callback(char *in, size_t block_size, size_t blocks, std::stringstream *out) {
    if(!out) return static_cast<size_t>(0);
    out->write(in, block_size * blocks);
    return block_size * blocks;
  }
  std::shared_ptr<CURL> connection;
  char error[CURL_ERROR_SIZE];
  std::stringstream result;
  std::default_random_engine generator;
  std::uniform_int_distribution<size_t> distribution;
};

std::string url(const std::string& path, const ptree& pt) {
  auto url = pt.get<std::string>("base_url") + path;
  auto key = pt.get_optional<std::string>("api_key");
  if(key)
    url += "&api_key=" + *key;
  return url;
}

//TODO: update this call to get only the tiles that have changed since last time
struct weighted_tile_t { GraphId t; size_t w; bool operator<(const weighted_tile_t& o) const { return w == o.w ? t < o.t : w < o.w; } };
std::priority_queue<weighted_tile_t> which_tiles(const ptree& pt) {
  //now real need to catch exceptions since we can't really proceed without this stuff
  LOG_INFO("Fetching transit feeds");
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  std::set<GraphId> tiles;
  const auto& tile_level = hierarchy.levels().rbegin()->second;
  curler_t curler;
  auto feeds = curler(pt.get<std::string>("base_url") + "/api/v1/feeds.geojson", "features");
  for(const auto& feature : feeds.get_child("features")) {
    //should be a polygon
    auto type = feature.second.get_optional<std::string>("geometry.type");
    if(!type || *type != "Polygon") {
      LOG_WARN("Skipping non-polygonal feature: " + feature.second.get_value<std::string>());
      continue;
    }
    //grab the tile row and column ranges for the max box around the polygon
    float min_x = 180, max_x = -180, min_y = 90, max_y = -90;
    for(const auto& coord :feature.second.get_child("geometry.coordinates").front().second) {
      auto x = coord.second.front().second.get_value<float>();
      auto y = coord.second.back().second.get_value<float>();
      if(x < min_x) min_x = x;
      if(x > max_x) max_x = x;
      if(y < min_y) min_y = y;
      if(y > max_y) max_y = y;
    }
    //convert coordinates to tile id bounding box accounting for geodesics
    min_y = std::min(min_y, PointLL(min_x, min_y).MidPoint({max_x, min_y}).second);
    max_y = std::max(max_y, PointLL(min_x, max_y).MidPoint({max_x, max_y}).second);
    auto min_c = tile_level.tiles.Col(min_x), min_r = tile_level.tiles.Row(min_y);
    auto max_c = tile_level.tiles.Col(max_x), max_r = tile_level.tiles.Row(max_y);
    if(min_c > max_c) std::swap(min_c, max_c);
    if(min_r > max_r) std::swap(min_r, max_r);
    //for each tile in the polygon figure out how heavy it is and keep track of it
    for(auto i = min_c; i <= max_c; ++i)
      for(auto j = min_r; j <= max_r; ++j)
        tiles.emplace(GraphId(tile_level.tiles.TileId(i,j), tile_level.level, 0));
  }
  //we want slowest to build tiles first, routes query is slowest so we weight by that
  //stop pairs is most numerous so that might want to be factored in as well
  std::priority_queue<weighted_tile_t> prioritized;
  auto now = time(nullptr);
  auto* utc = gmtime(&now); utc->tm_year += 1900; ++utc->tm_mon;
  for(const auto& tile : tiles) {
    auto bbox = tile_level.tiles.TileBounds(tile.tileid());
    auto min_y = std::max(bbox.miny(), bbox.minpt().MidPoint({bbox.maxx(), bbox.miny()}).second);
    auto max_y = std::min(bbox.maxy(), PointLL(bbox.minx(), bbox.maxy()).MidPoint(bbox.maxpt()).second);
    bbox = AABB2<PointLL>(bbox.minx(), min_y, bbox.maxx(), max_y);
    //stop count
    auto request = url((boost::format("/api/v1/stops?total=true&per_page=0&bbox=%1%,%2%,%3%,%4%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str(), pt);
    auto stops_total = curler(request, "meta.total").get<size_t>("meta.total");
    /*
    //route count
    request = url((boost::format("/api/v1/routes?total=true&per_page=0&bbox=%1%,%2%,%3%,%4%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str(), pt);
    auto routes_total = curler(request, "meta.total").get<size_t>("meta.total");
    //pair count
    request = url((boost::format("/api/v1/schedule_stop_pairs?total=true&per_page=0&bbox=%1%,%2%,%3%,%4%&service_from_date=%5%-%6%-%7%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy() % utc->tm_year % utc->tm_mon % utc->tm_mday).str(), pt);
    auto pairs_total = curler(request, "meta.total").get<size_t>("meta.total");
    */
    //we have anything we want it
    if(stops_total > 0/* || routes_total > 0|| pairs_total > 0*/) {
      prioritized.push(weighted_tile_t{tile, stops_total + 10/* + routes_total * 1000 + pairs_total*/}); //TODO: factor in stop pairs as well
      LOG_INFO(GraphTile::FileSuffix(tile, hierarchy) + " should have " + std::to_string(stops_total) +  " stops "/* +
          std::to_string(routes_total) +  " routes and " + std::to_string(pairs_total) +  " stop_pairs"*/);
    }
  }
  LOG_INFO("Finished with " + std::to_string(prioritized.size()) + " transit tiles in " +
           std::to_string(feeds.get_child("features").size()) + " feeds");
  return prioritized;
}

#define set_no_null(T, pt, path, null_value, set) {\
  auto value = pt.get<T>(path, null_value); \
  if(value != null_value) \
    set(value); \
}

void get_stops(Transit& tile, std::unordered_map<std::string, uint64_t>& stops,
    const GraphId& tile_id, const ptree& response, const AABB2<PointLL>& filter) {
  for(const auto& stop_pt : response.get_child("stops")) {
    const auto& ll_pt = stop_pt.second.get_child("geometry.coordinates");
    auto lon = ll_pt.front().second.get_value<float>();
    auto lat = ll_pt.back().second.get_value<float>();
    if(!filter.Contains({lon, lat}))
      continue;
    auto* stop = tile.add_stops();
    stop->set_lon(lon);
    stop->set_lat(lat);
    set_no_null(std::string, stop_pt.second, "onestop_id", "null", stop->set_onestop_id);
    set_no_null(std::string, stop_pt.second, "name", "null", stop->set_name);
    stop->set_wheelchair_boarding(stop_pt.second.get<bool>("tags.wheelchair_boarding", false));
    set_no_null(uint64_t, stop_pt.second, "tags.osm_way_id", 0, stop->set_osm_way_id);
    GraphId stop_id = tile_id;
    stop_id.fields.id = stops.size();
    stop->set_graphid(stop_id);
    stop->set_timezone(0);
    uint32_t timezone = DateTime::get_tz_db().to_index(stop_pt.second.get<std::string>("timezone", ""));
    if (timezone == 0)
      LOG_WARN("Timezone not found for stop " + stop->name());
    stop->set_timezone(timezone);
    stops.emplace(stop->onestop_id(), stop_id);
  }
}

void get_routes(Transit& tile, std::unordered_map<std::string, uint64_t>& routes,
    const std::unordered_map<std::string, std::string>& websites, const ptree& response) {
  for(const auto& route_pt : response.get_child("routes")) {
    auto* route = tile.add_routes();
    set_no_null(std::string, route_pt.second, "onestop_id", "null", route->set_onestop_id);
    std::string vehicle_type = route_pt.second.get<std::string>("tags.vehicle_type", "");
    Transit_VehicleType type = Transit_VehicleType::Transit_VehicleType_kRail;
    if (vehicle_type == "tram")
      type = Transit_VehicleType::Transit_VehicleType_kTram;
    else if (vehicle_type == "metro")
      type = Transit_VehicleType::Transit_VehicleType_kMetro;
    else if (vehicle_type == "rail")
      type = Transit_VehicleType::Transit_VehicleType_kRail;
    else if (vehicle_type == "bus")
      type = Transit_VehicleType::Transit_VehicleType_kBus;
    else if (vehicle_type == "ferry")
      type = Transit_VehicleType::Transit_VehicleType_kFerry;
    else if (vehicle_type == "cablecar")
      type = Transit_VehicleType::Transit_VehicleType_kCableCar;
    else if (vehicle_type == "gondola")
      type = Transit_VehicleType::Transit_VehicleType_kGondola;
    else if (vehicle_type == "funicular")
      type = Transit_VehicleType::Transit_VehicleType_kFunicular;
    else {
      LOG_ERROR("Skipping unsupported vehicle_type: " + vehicle_type + " for route " + route->onestop_id());
      tile.mutable_routes()->RemoveLast();
      continue;
    }
    route->set_vehicle_type(type);
    set_no_null(std::string, route_pt.second, "operated_by_onestop_id", "null", route->set_operated_by_onestop_id);
    set_no_null(std::string, route_pt.second, "operated_by_name", "null", route->set_operated_by_name);
    set_no_null(std::string, route_pt.second, "name", "null", route->set_name);
    set_no_null(std::string, route_pt.second, "tags.route_long_name", "null", route->set_route_long_name);
    set_no_null(std::string, route_pt.second, "tags.route_desc", "null", route->set_route_desc);
    std::string route_color = route_pt.second.get<std::string>("tags.route_color", "FFFFFF");
    std::string route_text_color = route_pt.second.get<std::string>("tags.route_text_color", "000000");
    boost::algorithm::trim(route_color);
    boost::algorithm::trim(route_text_color);
    route_color = (route_color == "null" ? "FFFFFF" : route_color);
    route_text_color = (route_text_color == "null" ? "000000" : route_text_color);
    auto website = websites.find(route->operated_by_onestop_id());
    if(website != websites.cend())
      route->set_operated_by_website(website->second);
    route->set_route_color(strtol(route_color.c_str(), nullptr, 16));
    route->set_route_text_color(strtol(route_text_color.c_str(), nullptr, 16));
    routes.emplace(route->onestop_id(), routes.size());
  }
}

struct unique_transit_t {
  std::mutex lock;
  std::unordered_map<std::string, size_t> trips;
  std::unordered_map<std::string, size_t> block_ids;
  std::unordered_set<std::string> missing_routes;
  std::unordered_map<std::string, size_t> lines;
};

bool get_stop_pairs(Transit& tile, unique_transit_t& uniques, const ptree& response,
    const std::unordered_map<std::string, uint64_t>& stops, const std::unordered_map<std::string, size_t>& routes) {
  bool dangles = false;
  for(const auto& pair_pt : response.get_child("schedule_stop_pairs")) {
    auto* pair = tile.add_stop_pairs();

    //origin
    pair->set_origin_onestop_id(pair_pt.second.get<std::string>("origin_onestop_id"));
    auto origin = stops.find(pair->origin_onestop_id());
    if(origin != stops.cend())
      pair->set_origin_graphid(origin->second);
    else
      dangles = true;

    //destination
    pair->set_destination_onestop_id(pair_pt.second.get<std::string>("destination_onestop_id"));
    auto destination = stops.find(pair->destination_onestop_id());
    if(destination != stops.cend())
      pair->set_destination_graphid(destination->second);
    else
      dangles = true;

    //route
    auto route_id = pair_pt.second.get<std::string>("route_onestop_id");
    auto route = routes.find(route_id);
    if(route == routes.cend()) {
      uniques.lock.lock();
      if(uniques.missing_routes.find(route_id) == uniques.missing_routes.cend()) {
        LOG_ERROR("No route " + route_id);
        uniques.missing_routes.emplace(route_id);
      }
      uniques.lock.unlock();
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    pair->set_route_index(route->second);

    //uniq line id
    auto line_id = pair->origin_onestop_id() < pair->destination_onestop_id() ?
                    pair->origin_onestop_id() + pair->destination_onestop_id() + route_id:
                    pair->destination_onestop_id() + pair->origin_onestop_id() + route_id;
    uniques.lock.lock();
    auto inserted = uniques.lines.insert({line_id, uniques.lines.size()});
    pair->set_line_id(inserted.first->second);
    uniques.lock.unlock();

    //timing information
    auto origin_time = pair_pt.second.get<std::string>("origin_departure_time", "null");
    auto dest_time = pair_pt.second.get<std::string>("destination_arrival_time", "null");
    auto start_date = pair_pt.second.get<std::string>("service_start_date", "null");
    auto end_date = pair_pt.second.get<std::string>("service_end_date", "null");
    if (origin_time == "null" || dest_time == "null" || start_date == "null" || end_date == "null") {
      LOG_ERROR("Missing timing information: " + pair->origin_onestop_id() + " --> " + pair->destination_onestop_id());
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    pair->set_origin_departure_time(DateTime::seconds_from_midnight(origin_time));
    pair->set_destination_arrival_time(DateTime::seconds_from_midnight(dest_time));
    pair->set_service_start_date(DateTime::get_formatted_date(start_date).julian_day());
    pair->set_service_end_date(DateTime::get_formatted_date(end_date).julian_day());
    for(const auto& service_days : pair_pt.second.get_child("service_days_of_week")) {
      pair->add_service_days_of_week(service_days.second.get_value<bool>());
      //TODO: if none of these were true we should skip
    }

    //trip
    std::string trip = pair_pt.second.get<std::string>("trip", "null");
    if (trip == "null") {
      LOG_ERROR("No trip for pair: " + pair->origin_onestop_id() + " --> " + pair->destination_onestop_id());
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    uniques.lock.lock();
    inserted = uniques.trips.insert({trip, uniques.trips.size()});
    pair->set_trip_key(inserted.first->second);
    uniques.lock.unlock();

    //block id
    std::string block_id = pair_pt.second.get<std::string>("block_id", "null");
    if(block_id == "null") {
      pair->set_block_id(0);
    }
    else {
      uniques.lock.lock();
      inserted = uniques.block_ids.insert({block_id, uniques.block_ids.size()});
      pair->set_block_id(inserted.first->second);
      uniques.lock.unlock();
    }

    pair->set_wheelchair_accessible(pair_pt.second.get<bool>("wheelchair_accessible", false));
    uint32_t timezone = DateTime::get_tz_db().to_index(pair_pt.second.get<std::string>("origin_timezone", ""));
    if (timezone == 0)
      LOG_WARN("Timezone not found for stop_pair: " + pair->origin_onestop_id() + " --> " + pair->destination_onestop_id());
    pair->set_origin_timezone(timezone);

    set_no_null(std::string, pair_pt.second, "trip_headsign", "null", pair->set_trip_headsign);
    pair->set_bikes_allowed(pair_pt.second.get<bool>("bikes_allowed", false));

    const auto& except_dates = pair_pt.second.get_child_optional("service_except_dates");
    if (except_dates && !except_dates->empty()) {
      for(const auto& service_except_dates : pair_pt.second.get_child("service_except_dates")) {
        auto d = DateTime::get_formatted_date(service_except_dates.second.get_value<std::string>());
        pair->add_service_except_dates(d.julian_day());
      }
    }

    const auto& added_dates = pair_pt.second.get_child_optional("service_added_dates");
    if (added_dates && !added_dates->empty()) {
      for(const auto& service_added_dates : pair_pt.second.get_child("service_added_dates")) {
        auto d = DateTime::get_formatted_date(service_added_dates.second.get_value<std::string>());
        pair->add_service_added_dates(d.julian_day());
      }
    }
    //TODO: copy rest of attributes
  }
  return dangles;
}

void fetch_tiles(const ptree& pt, std::priority_queue<weighted_tile_t>& queue, unique_transit_t& uniques, std::promise<std::list<GraphId> >& promise) {
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  const auto& tiles = hierarchy.levels().rbegin()->second.tiles;
  std::list<GraphId> dangling;
  curler_t curler;
  auto now = time(nullptr);
  auto* utc = gmtime(&now); utc->tm_year += 1900; ++utc->tm_mon; //TODO: use timezone code?

  //for each tile
  while(true) {
    GraphId current;
    uniques.lock.lock();
    if(queue.empty()) {
      uniques.lock.unlock();
      break;
    }
    current = queue.top().t;
    queue.pop();
    uniques.lock.unlock();
    auto filter = tiles.TileBounds(current.tileid());
    //account for geodesics
    auto min_y = std::max(filter.miny(), filter.minpt().MidPoint({filter.maxx(), filter.miny()}).second);
    auto max_y = std::min(filter.maxy(), PointLL(filter.minx(), filter.maxy()).MidPoint(filter.maxpt()).second);
    AABB2<PointLL> bbox(filter.minx(), min_y, filter.maxx(), max_y);
    ptree response;
    Transit tile;
    auto file_name = GraphTile::FileSuffix(current, hierarchy);
    file_name = file_name.substr(0, file_name.size() - 3) + "pbf";
    boost::filesystem::path transit_tile = pt.get<std::string>("mjolnir.transit_dir") + '/' + file_name;
    LOG_INFO("Fetching " + transit_tile.string());

    //pull out all the STOPS (you see what we did there?)
    std::unordered_map<std::string, uint64_t> stops;
    boost::optional<std::string> request = url((boost::format("/api/v1/stops?total=false&per_page=%1%&bbox=%2%,%3%,%4%,%5%")
      % pt.get<std::string>("per_page") % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str(), pt);
    while(request) {
      //grab some stuff
      response = curler(*request, "stops");
      //copy stops in, keeping map of stopid to graphid
      get_stops(tile, stops, current, response, filter);
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }
    //um yeah.. we need these
    if(stops.size() == 0) {
      LOG_WARN(transit_tile.string() + " had no stops and will not be stored");
      continue;
    }

    //pull out all operator WEBSITES
    request = url((boost::format("/api/v1/operators?total=false&per_page=%1%&bbox=%2%,%3%,%4%,%5%")
      % pt.get<std::string>("per_page") % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str(), pt);
    std::unordered_map<std::string, std::string> websites;
    while(request) {
      //grab some stuff
      response = curler(*request, "operators");
      //save the websites to a map
      for(const auto& operators_pt : response.get_child("operators")) {
        std::string onestop_id = operators_pt.second.get<std::string>("onestop_id", "");
        std::string website = operators_pt.second.get<std::string>("website", "");
        if(!onestop_id.empty() && onestop_id != "null" && !website.empty() && website != "null")
          websites.emplace(onestop_id, website);
      }
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //pull out all ROUTES
    request = url((boost::format("/api/v1/routes?total=false&per_page=%1%&bbox=%2%,%3%,%4%,%5%")
      % pt.get<std::string>("per_page") % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str(), pt);
    std::unordered_map<std::string, size_t> routes;
    while(request) {
      //grab some stuff
      uniques.lock.lock();
      response = curler(*request, "routes");
      uniques.lock.unlock();
      //copy routes in, keeping track of routeid to route index
      get_routes(tile, routes, websites, response);
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //pull out all SCHEDULE_STOP_PAIRS
    bool dangles = false;
    request = url((boost::format("/api/v1/schedule_stop_pairs?total=false&per_page=%1%&bbox=%2%,%3%,%4%,%5%&service_from_date=%6%-%7%-%8%")
      % pt.get<std::string>("per_page") % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy() % utc->tm_year % utc->tm_mon % utc->tm_mday).str(), pt);
    while(request) {
      //grab some stuff
      response = curler(*request, "schedule_stop_pairs");
      //copy pairs in, noting if any dont have stops
      dangles = get_stop_pairs(tile, uniques, response, stops, routes) || dangles;
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //remember who dangles
    if(dangles)
      dangling.emplace_back(current);

    //write pbf to file
    if (!boost::filesystem::exists(transit_tile.parent_path()))
      boost::filesystem::create_directories(transit_tile.parent_path());
    std::fstream stream(transit_tile.string(), std::ios::out | std::ios::trunc | std::ios::binary);
    tile.SerializeToOstream(&stream);
    LOG_INFO(transit_tile.string() + " had " + std::to_string(tile.stops_size()) + " stops " +
      std::to_string(tile.routes_size()) + " routes " + std::to_string(tile.stop_pairs_size()) + " stop pairs");
  }

  //give back the work for later
  promise.set_value(dangling);
}

std::list<GraphId> fetch(const ptree& pt, std::priority_queue<weighted_tile_t>& tiles,
    unsigned int thread_count = std::max(static_cast<unsigned int>(1), std::thread::hardware_concurrency())) {
  LOG_INFO("Fetching " + std::to_string(tiles.size()) + " transit tiles with " + std::to_string(thread_count) + " threads...");

  //schedule some work
  unique_transit_t uniques;
  std::vector<std::shared_ptr<std::thread> > threads(thread_count);
  std::vector<std::promise<std::list<GraphId> > > promises(threads.size());
  for (size_t i = 0; i < threads.size(); ++i)
    threads[i].reset(new std::thread(fetch_tiles, std::cref(pt), std::ref(tiles), std::ref(uniques), std::ref(promises[i])));

  //let the threads finish and get the dangling list
  for (auto& thread : threads)
    thread->join();
  std::list<GraphId> dangling;
  for (auto& promise : promises) {
    try {
      dangling.splice(dangling.end(), promise.get_future().get());
    }
    catch(std::exception& e) {
      //TODO: throw further up the chain?
    }
  }

  LOG_INFO("Finished");
  return dangling;
}

GraphId id(const boost::property_tree::ptree& pt, const std::string& transit_tile) {
  auto tile_dir = pt.get<std::string>("mjolnir.hierarchy.tile_dir");
  auto transit_dir = pt.get<std::string>("mjolnir.transit_dir");
  auto graph_tile = tile_dir + transit_tile.substr(transit_dir.size());
  boost::algorithm::trim_if(graph_tile, boost::is_any_of(".pbf"));
  graph_tile += ".gph";
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  return GraphTile::GetTileId(graph_tile, hierarchy);
}

Transit read_pbf(const std::string& file_name, std::mutex& lock) {
  lock.lock();
  std::fstream file(file_name, std::ios::in | std::ios::binary);
  if(!file) {
    throw std::runtime_error("Couldn't load " + file_name);
    lock.unlock();
  }
  std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  lock.unlock();
  google::protobuf::io::ArrayInputStream as(static_cast<const void*>(buffer.c_str()), buffer.size());
  google::protobuf::io::CodedInputStream cs(static_cast<google::protobuf::io::ZeroCopyInputStream*>(&as));
  cs.SetTotalBytesLimit(buffer.size() * 2, buffer.size() * 2);
  Transit transit;
  if(!transit.ParseFromCodedStream(&cs))
    throw std::runtime_error("Couldn't load " + file_name);
  return transit;
}

struct dist_sort_t {
  PointLL center;
  Tiles<PointLL> grid;
  dist_sort_t(const GraphId& center, const Tiles<PointLL>& grid):grid(grid) {
    this->center = grid.TileBounds(center.tileid()).Center();
  }
  bool operator()(const GraphId& a, const GraphId& b) const {
    auto a_dist = center.Distance(grid.TileBounds(a.tileid()).Center());
    auto b_dist = center.Distance(grid.TileBounds(b.tileid()).Center());
    if(a_dist == b_dist)
      return a.tileid() < b.tileid();
    return a_dist < b_dist;
  }
};

void stitch_tiles(const ptree& pt, const std::unordered_set<GraphId>& all_tiles, std::list<GraphId>& tiles, std::mutex& lock) {
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  auto grid = hierarchy.levels().rbegin()->second.tiles;
  auto tile_name = [&hierarchy, &pt](const GraphId& id){
    auto file_name = GraphTile::FileSuffix(id, hierarchy);
    file_name = file_name.substr(0, file_name.size() - 3) + "pbf";
    return pt.get<std::string>("mjolnir.transit_dir") + '/' + file_name;
  };

  //for each tile
  while(true) {
    GraphId current;
    lock.lock();
    if(tiles.empty()) {
      lock.unlock();
      break;
    }
    current = tiles.front();
    tiles.pop_front();
    lock.unlock();

    //open tile make a hash of missing stop to invalid graphid
    auto file_name = tile_name(current);
    auto tile = read_pbf(file_name, lock);
    std::unordered_map<std::string, GraphId> needed;
    for(const auto& stop_pair : tile.stop_pairs()) {
      if(!stop_pair.has_origin_graphid())
        needed.emplace(stop_pair.origin_onestop_id(), GraphId{});
      if(!stop_pair.has_destination_graphid())
        needed.emplace(stop_pair.destination_onestop_id(), GraphId{});
    }

    //do while we have more to find and arent sick of searching
    std::set<GraphId, dist_sort_t> unchecked(all_tiles.cbegin(), all_tiles.cend(), dist_sort_t(current, grid));
    size_t found = 0;
    while(found < needed.size() && unchecked.size()) {
      //crack it open to see if it has what we want
      auto neighbor_id = *unchecked.cbegin();
      unchecked.erase(unchecked.begin());
      if(neighbor_id != current) {
        auto neighbor_file_name = tile_name(neighbor_id);
        auto neighbor = read_pbf(neighbor_file_name, lock);
        for(const auto& stop : neighbor.stops()) {
          auto stop_itr = needed.find(stop.onestop_id());
          if(stop_itr != needed.cend()) {
            stop_itr->second.value = stop.graphid();
            ++found;
          }
        }
      }
    }

    //get the ids fixed up and write pbf to file
    std::unordered_set<std::string> not_found;
    for(auto& stop_pair : *tile.mutable_stop_pairs()) {
      if(!stop_pair.has_origin_graphid()) {
        auto found = needed.find(stop_pair.origin_onestop_id())->second;
        if(found.Is_Valid())
          stop_pair.set_origin_graphid(found);
        else if(not_found.find(stop_pair.origin_onestop_id()) == not_found.cend()) {
          LOG_ERROR("Stop not found: " + stop_pair.origin_onestop_id());
          not_found.emplace(stop_pair.origin_onestop_id());
        }
        //else{ TODO: we could delete this stop pair }
      }
      if(!stop_pair.has_destination_graphid()) {
        auto found = needed.find(stop_pair.destination_onestop_id())->second;
        if(found.Is_Valid())
          stop_pair.set_destination_graphid(found);
        else if(not_found.find(stop_pair.destination_onestop_id()) == not_found.cend()) {
          LOG_ERROR("Stop not found: " + stop_pair.destination_onestop_id());
          not_found.emplace(stop_pair.destination_onestop_id());
        }
        //else{ TODO: we could delete this stop pair }
      }
    }
    lock.lock();
    std::fstream stream(file_name, std::ios::out | std::ios::trunc | std::ios::binary);
    tile.SerializeToOstream(&stream);
    lock.unlock();
    LOG_INFO(file_name + " stitched " + std::to_string(found) + " of " + std::to_string(needed.size()) + " stops");
  }
}

void stitch(const ptree& pt, const std::unordered_set<GraphId>& all_tiles, std::list<GraphId>& dangling_tiles,
    unsigned int thread_count = std::max(static_cast<unsigned int>(1), std::thread::hardware_concurrency())) {
  LOG_INFO("Stitching " + std::to_string(dangling_tiles.size()) + " transit tiles with " + std::to_string(thread_count) + " threads...");

  //figure out where the work should go
  std::vector<std::shared_ptr<std::thread> > threads(thread_count);
  std::mutex lock;

  //make let them rip
  for (size_t i = 0; i < threads.size(); ++i)
    threads[i].reset(new std::thread(stitch_tiles, std::cref(pt), std::cref(all_tiles), std::ref(dangling_tiles), std::ref(lock)));

  //wait for them to finish
  for (auto& thread : threads)
    thread->join();

  LOG_INFO("Finished");
}

int main(int argc, char** argv) {
  if(argc < 2) {
    std::cerr << "Usage: " << std::string(argv[0]) << " valhalla_config transit_land_url per_page transit_land_api_key" << std::endl;
    std::cerr << "Sample: " << std::string(argv[0]) << " conf/valhalla.json http://transit.land/ 1000 transitland-YOUR_KEY_SUFFIX" << std::endl;
    return 1;
  }

  //args and config file loading
  ptree pt;
  boost::property_tree::read_json(std::string(argv[1]), pt);
  pt.add("base_url", std::string(argv[2]));
  pt.add("per_page", argc > 3 ? std::string(argv[3]) : std::to_string(1000));
  if(argc > 4)
    pt.add("api_key", std::string(argv[4]));

  //yes we want to curl
  curl_global_init(CURL_GLOBAL_DEFAULT);

  //go get information about what transit tiles we should be fetching
  auto transit_tiles = which_tiles(pt);

  //spawn threads to download all the tiles returning a list of
  //tiles that ended up having dangling stop pairs
  auto dangling_tiles = fetch(pt, transit_tiles);
  curl_global_cleanup();

  //figure out which transit tiles even exist
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  boost::filesystem::recursive_directory_iterator transit_file_itr(pt.get<std::string>("mjolnir.transit_dir") + '/' + std::to_string(hierarchy.levels().rbegin()->first));
  boost::filesystem::recursive_directory_iterator end_file_itr;
  std::unordered_set<GraphId> all_tiles;
  for(; transit_file_itr != end_file_itr; ++transit_file_itr)
    if(boost::filesystem::is_regular(transit_file_itr->path()) && transit_file_itr->path().extension() == ".pbf")
      all_tiles.emplace(id(pt, transit_file_itr->path().string()));

  //spawn threads to connect dangling stop pairs to adjacent tiles' stops
  stitch(pt, all_tiles, dangling_tiles);

  return 0;
}