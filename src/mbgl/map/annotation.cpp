#include <mbgl/map/annotation.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/live_tile.hpp>
#include <mbgl/util/ptr.hpp>
#include <mbgl/util/std.hpp>

#include <algorithm>
#include <memory>

namespace mbgl {

enum class AnnotationType : uint8_t {
    Point,
    Shape
};

using AnnotationSegment = std::vector<LatLng>;
using AnnotationSegments = std::vector<AnnotationSegment>;

class Annotation : private util::noncopyable {
    friend class AnnotationManager;
public:
    Annotation(AnnotationType, const AnnotationSegments&);

private:
    LatLng getPoint() const;
    LatLngBounds getBounds() const { return bounds; }

private:
    const AnnotationType type = AnnotationType::Point;
    const AnnotationSegments geometry;
    std::map<Tile::ID, std::vector<std::weak_ptr<const LiveTileFeature>>> tileFeatures;
    const LatLngBounds bounds;
};


Annotation::Annotation(AnnotationType type_, const AnnotationSegments& geometry_)
    : type(type_),
      geometry(geometry_),
      bounds([this] {
          LatLngBounds bounds_;
          if (type == AnnotationType::Point) {
              bounds_ = { getPoint(), getPoint() };
          } else {
              for (auto& segment : geometry) {
                  for (auto& point : segment) {
                      bounds_.extend(point);
                  }
              }
          }
          return bounds_;
      }()) {
}

LatLng Annotation::getPoint() const {
    assert(!geometry.empty());
    assert(!geometry[0].empty());
    return geometry[0][0];
}

AnnotationManager::AnnotationManager() {}

AnnotationManager::~AnnotationManager() {
    // leave this here because the header file doesn't have a definition of
    // Annotation so we can't destruct the object with just the header file.
}

void AnnotationManager::setDefaultPointAnnotationSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mtx);
    defaultPointAnnotationSymbol = symbol;
}

uint32_t AnnotationManager::nextID() {
    return nextID_++;
}

vec2<double> AnnotationManager::projectPoint(const LatLng& point) {
    const double sine = std::sin(point.latitude * M_PI / 180.0);
    const double x = point.longitude / 360.0 + 0.5;
    const double y = 0.5 - 0.25 * std::log((1.0 + sine) / (1.0 - sine)) / M_PI;
    return { x, y };
}

std::pair<std::vector<Tile::ID>, AnnotationIDs> AnnotationManager::addPointAnnotations(
    const std::vector<LatLng>& points, const std::vector<std::string>& symbols, const Map& map) {
    std::lock_guard<std::mutex> lock(mtx);

    const uint16_t extent = 4096;

    AnnotationIDs annotationIDs(points.size());
    std::vector<Tile::ID> affectedTiles;

    for (size_t i = 0; i < points.size(); ++i) {
        const uint32_t annotationID = nextID();

        auto anno_it = annotations.emplace(
            annotationID,
            util::make_unique<Annotation>(AnnotationType::Point,
                                          AnnotationSegments({ { points[i] } })));

        const uint8_t maxZoom = map.getMaxZoom();

        uint32_t z2 = 1 << maxZoom;

        const vec2<double> p = projectPoint(points[i]);

        uint32_t x = p.x * z2;
        uint32_t y = p.y * z2;

        for (int8_t z = maxZoom; z >= 0; z--) {
            affectedTiles.emplace_back(z, x, y);
            Tile::ID tileID = affectedTiles.back();

            const Coordinate coordinate(extent * (p.x * z2 - x), extent * (p.y * z2 - y));

            const GeometryCollection geometries({ { { { coordinate } } } });

            const std::map<std::string, std::string> properties = {
                { "sprite", (symbols[i].length() ? symbols[i] : defaultPointAnnotationSymbol) }
            };

            auto feature =
                std::make_shared<const LiveTileFeature>(FeatureType::Point, geometries, properties);

            auto tile_it = annotationTiles.find(tileID);
            if (tile_it != annotationTiles.end()) {
                // get point layer & add feature
                auto layer =
                    tile_it->second.second->getMutableLayer(layerID);
                layer->addFeature(feature);
                // record annotation association with tile
                tile_it->second.first.push_back(annotationID);
            } else {
                // create point layer & add feature
                util::ptr<LiveTileLayer> layer = std::make_shared<LiveTileLayer>();
                layer->addFeature(feature);
                // create tile & record annotation association
                auto tile_pos = annotationTiles.emplace(
                    tileID, std::make_pair(AnnotationIDs({ annotationID }),
                                           util::make_unique<LiveTile>()));
                // add point layer to tile
                tile_pos.first->second.second->addLayer(layerID, layer);
            }

            // record annotation association with tile feature
            anno_it.first->second->tileFeatures.emplace(
                tileID, std::vector<std::weak_ptr<const LiveTileFeature>>({ feature }));

            z2 /= 2;
            x /= 2;
            y /= 2;
        }

        annotationIDs.push_back(annotationID);
    }

    return std::make_pair(affectedTiles, annotationIDs);
}

std::vector<Tile::ID> AnnotationManager::removeAnnotations(const AnnotationIDs& ids) {
    std::lock_guard<std::mutex> lock(mtx);

    std::vector<Tile::ID> affectedTiles;

    for (auto& annotationID : ids) {
        const auto annotation_it = annotations.find(annotationID);
        if (annotation_it != annotations.end()) {
            auto& annotation = annotation_it->second;
            for (auto& tile_it : annotationTiles) {
                auto& tileAnnotations = tile_it.second.first;
                util::erase_if(tileAnnotations, tileAnnotations.begin(), tileAnnotations.end(),
                               [&](const uint32_t annotationID_)
                                   -> bool { return (annotationID_ == annotationID); });
                auto features_it = annotation->tileFeatures.find(tile_it.first);
                if (features_it != annotation->tileFeatures.end()) {
                    const auto layer =
                        tile_it.second.second->getMutableLayer(layerID);
                    const auto& features = features_it->second;
                    assert(!features.empty());
                    layer->removeFeature(features[0]);
                    affectedTiles.push_back(tile_it.first);
                }
            }
            annotations.erase(annotationID);
        }
    }

    return affectedTiles;
}

std::vector<uint32_t> AnnotationManager::getAnnotationsInBounds(const LatLngBounds& queryBounds,
                                                                const Map& map) const {
    std::lock_guard<std::mutex> lock(mtx);

    const uint8_t z = map.getMaxZoom();
    const uint32_t z2 = 1 << z;
    const vec2<double> swPoint = projectPoint(queryBounds.sw);
    const vec2<double> nePoint = projectPoint(queryBounds.ne);

    // tiles number y from top down
    const Tile::ID nwTile(z, swPoint.x * z2, nePoint.y * z2);
    const Tile::ID seTile(z, nePoint.x * z2, swPoint.y * z2);

    std::vector<uint32_t> matchingAnnotations;

    for (auto& tile : annotationTiles) {
        Tile::ID id = tile.first;
        if (id.z == z) {
            if (id.x >= nwTile.x && id.x <= seTile.x && id.y >= nwTile.y && id.y <= seTile.y) {
                if (id.x > nwTile.x && id.x < seTile.x && id.y > nwTile.y && id.y < seTile.y) {
                    // trivial accept; grab all of the tile's annotations
                    std::copy(tile.second.first.begin(), tile.second.first.end(),
                              std::back_inserter(matchingAnnotations));
                } else {
                    // check tile's annotations' bounding boxes
                    std::copy_if(tile.second.first.begin(), tile.second.first.end(),
                                 std::back_inserter(matchingAnnotations),
                                 [&](const uint32_t annotationID) -> bool {
                        const auto it = annotations.find(annotationID);
                        if (it != annotations.end()) {
                            const LatLngBounds annoBounds = it->second->getBounds();
                            return (annoBounds.sw.latitude >= queryBounds.sw.latitude &&
                                    annoBounds.ne.latitude <= queryBounds.ne.latitude &&
                                    annoBounds.sw.longitude >= queryBounds.sw.longitude &&
                                    annoBounds.ne.longitude <= queryBounds.ne.longitude);
                        } else {
                            return false;
                        }
                    });
                }
            }
        }
    }

    return matchingAnnotations;
}

LatLngBounds AnnotationManager::getBoundsForAnnotations(const AnnotationIDs& ids) const {
    std::lock_guard<std::mutex> lock(mtx);

    LatLngBounds bounds;
    for (auto id : ids) {
        const auto annotation_it = annotations.find(id);
        if (annotation_it != annotations.end()) {
            bounds.extend(annotation_it->second->getPoint());
        }
    }

    return bounds;
}

const LiveTile* AnnotationManager::getTile(Tile::ID const& id) {
    std::lock_guard<std::mutex> lock(mtx);

    const auto tile_it = annotationTiles.find(id);
    if (tile_it != annotationTiles.end()) {
        return tile_it->second.second.get();
    }
    return nullptr;
}

const std::string AnnotationManager::layerID = "com.mapbox.annotations.points";


}
