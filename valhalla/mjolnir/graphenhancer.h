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
   * @param pt          property tree containing the hierarchy configuration
   * @param access_file where to store the nodes so they are not in memory
   * @param vias        updated vias (wayids were updated in the builder to graphids)
   * @param res_ids     updated to and from ids (wayids were updated in the builder to graphids)
   */
  static void Enhance(const boost::property_tree::ptree& pt,
                      const std::string& access_file,
                      const std::vector<uint64_t> vias,
                      const std::vector<uint64_t> res_ids);

};

}
}

#endif  // VALHALLA_MJOLNIR_GRAPHENHANCER_H
