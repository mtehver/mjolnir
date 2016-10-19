#ifndef VALHALLA_MJOLNIR_UTIL_H_
#define VALHALLA_MJOLNIR_UTIL_H_

#include <vector>
#include <string>

#include <boost/property_tree/ptree.hpp>

namespace valhalla {
namespace mjolnir {

/**
 * Creates the tile storage handler for specified configuration.
 * @param  pt The configuration to use.
 * @return The storage handler instance.
 */
std::shared_ptr<valhalla::baldr::GraphTileStorage> CreateTileStorage(const boost::property_tree::ptree& pt, const std::string& key = "mjolnir.tile_dir");

/**
 * Splits a tag into a vector of strings.
 * @param  tag_value  tag to split
 * @param  delim      defaults to ;
 * @return the vector of strings
*/
std::vector<std::string> GetTagTokens(const std::string& tag_value,
                                      char delim = ';');

}
}
#endif  // VALHALLA_MJOLNIR_UTIL_H_
