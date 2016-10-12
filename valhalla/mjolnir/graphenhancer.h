#ifndef VALHALLA_MJOLNIR_GRAPHENHANCER_H
#define VALHALLA_MJOLNIR_GRAPHENHANCER_H

#include <boost/property_tree/ptree.hpp>

namespace valhalla {
namespace mjolnir {

/**
 * Class used to enhance graph tile information at the local level.
 */
class GraphEnhancer {
 public:

  /**
   * Enhance the local level graph tile information.
   * @param pt                        property tree containing the hierarchy configuration
   * @param access_file               where to store the access tags so they are not in memory
   * @param complex_restriction_file  where to store the complex restriction so they are not in memory
   * @param end_map                   map of ids that are at the end of a restriction
   */
  static void Enhance(const boost::property_tree::ptree& pt,
                      const std::string& access_file,
                      const std::string& complex_restriction_file,
                      const std::unordered_multimap<uint64_t, uint64_t>& end_map);

};

}
}

#endif  // VALHALLA_MJOLNIR_GRAPHENHANCER_H
