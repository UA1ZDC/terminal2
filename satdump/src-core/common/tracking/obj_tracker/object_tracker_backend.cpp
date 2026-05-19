#include "object_tracker.h"
#include "coord_system_rotator.h"
#include "common/geodetic/geodetic_coordinates.h"
#include "common/utils.h"
#include "logger.h"
#include "common/tracking/tle.h"
#include <cfloat>

namespace satdump
{
    void ObjectTracker::backend_run()
    {
        while (backend_should_run)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (!has_tle)
                continue;

            general_mutex.lock();

            double current_time = getTime() + static_cast<double>(time_delta) / 1e3;

            if (tracking_mode == TRACKING_HORIZONS)
            {
                if (current_time > last_horizons_fetch_time + 3600)
                {
                    loadHorizons(current_time);
                    updateNextPass(current_time);
                    backend_needs_update = false;
                }

                if (horizons_data.size() > 0)
                {
                    //    size_t iter = 0;
                    //    for (size_t i = 0; i < horizons_data.size(); i++)
                    //        if (horizons_data[i].timestamp < current_time)
                    //            iter = i;

                    if (current_time > next_los_time)
                        updateNextPass(current_time);

                    horizons_interpolate(current_time, &sat_current_pos.az, &sat_current_pos.el);
                }
            }
            else if (tracking_mode == TRACKING_SATELLITE)
            {
                if (satellite_object != nullptr)
                {
                    predict_orbit(satellite_object, &satellite_orbit, predict_to_julian_double(current_time));
                    Vector3 tilt = {rotator_correction_az, rotator_correction_el, rotator_correction_el_dir};
                    predict_observe_orbit_new(tilt, satellite_observer_station, &satellite_orbit, &satellite_observation_pos);

                    if (current_time > next_los_time)
                        updateNextPass(current_time);

                    sat_current_pos.az = satellite_observation_pos.azimuth * RAD_TO_DEG;
                    sat_current_pos.el = satellite_observation_pos.elevation * RAD_TO_DEG;
                    sat_current_vel.az = satellite_observation_pos.azimuth_rate * RAD_TO_DEG;
                    sat_current_vel.el = satellite_observation_pos.elevation_rate * RAD_TO_DEG;
                }
            }

            // Update
            if (backend_needs_update)
            {
                logger->trace("Updating elements...");

                if (tracking_mode == TRACKING_HORIZONS)
                {
                    loadHorizons(current_time);
                    updateNextPass(current_time);
                }
                else if (tracking_mode == TRACKING_SATELLITE)
                {
                    if (satellite_object != nullptr)
                        predict_destroy_orbital_elements(satellite_object);

                    auto &tle = (*general_tle_registry)[current_satellite_id];

                    satellite_object = predict_parse_tle(tle.line1.c_str(), tle.line2.c_str());
                    updateNextPass(current_time);
                }

                backend_needs_update = false;
            }

            general_mutex.unlock();
        }
    }

    void ObjectTracker::updateNextPass(double current_time)
    {
        upcoming_passes_mtx.lock();
        logger->trace("Update pass trajectory...");

        upcoming_pass_points.clear();
        min_correction_azimuth = rotator_az_min;
        max_correction_azimuth = rotator_az_max;
        next_aos_time = 0;
        next_los_time = 0;
        northbound_cross = false;
        southbound_cross = false;

        if (tracking_mode == TRACKING_HORIZONS)
        {
            if (horizons_data.size() == 0)
            {
                upcoming_passes_mtx.unlock();
                return;
            }

            int iter = 0;
            for (int i = 0; i < (int)horizons_data.size(); i++)
                if (horizons_data[i].timestamp < current_time)
                    iter = i;

            if (horizons_data[iter].el > 0) // Already got AOS
            {
                next_aos_time = current_time;

                for (int i = iter - 1; i >= 0; i--) // Attempt to find previous AOS
                {
                    if (horizons_data[i].el <= 0)
                    {
                        next_aos_time = horizons_data[i].timestamp;
                        sat_next_aos_pos.az = horizons_data[i].az;
                        sat_next_aos_pos.el = 0;
                        break;
                    }
                }

                for (int i = iter; i < (int)horizons_data.size(); i++) // Find LOS
                {
                    if (horizons_data[i].el <= 0)
                    {
                        next_los_time = horizons_data[i].timestamp;
                        break;
                    }
                }
            }
            else
            {
                int aos_iter = 0;
                for (int i = iter; i < (int)horizons_data.size(); i++) // Find AOS
                {
                    if (horizons_data[i].el > 0)
                    {
                        next_aos_time = horizons_data[i].timestamp;
                        sat_next_aos_pos.az = horizons_data[i].az;
                        sat_next_aos_pos.el = 0;
                        aos_iter = i;
                        break;
                    }
                }

                if (next_aos_time != 0)
                {
                    for (int i = aos_iter; i < (int)horizons_data.size(); i++) // Find LOS
                    {
                        if (horizons_data[i].el <= 0)
                        {
                            next_los_time = horizons_data[i].timestamp;
                            break;
                        }
                    }
                }
            }

            if (/*is_gui &&*/ next_aos_time != 0 && next_los_time != 0)
            {
                double time_step = abs(next_los_time - next_aos_time) / 50.0;

                for (double ctime = next_aos_time; ctime <= next_los_time; ctime += time_step)
                {
                    int iter = 0;
                    for (int i = 0; i < (int)horizons_data.size(); i++)
                        if (horizons_data[i].timestamp < ctime)
                            iter = i;

                    upcoming_pass_points.push_back({horizons_data[iter].az, horizons_data[iter].el});
                }
            }
        }
        else if (tracking_mode == TRACKING_SATELLITE)
        {
            if (predict_is_geosynchronous(satellite_object))
            {
                next_aos_time = 0;
                next_los_time = DBL_MAX;
                min_correction_azimuth = rotator_az_min;
                max_correction_azimuth = rotator_az_max;
                upcoming_passes_mtx.unlock();
                return;
            }

            // Get next LOS
            predict_observation next_aos, next_los;
            next_aos = next_los = predict_next_los(satellite_observer_station, satellite_object, predict_to_julian_double(getTime()));

            // Calculate the AOS before that LOS
            next_aos_time = next_los_time = predict_from_julian(next_los.time);
            do
            {
                next_aos = predict_next_aos(satellite_observer_station, satellite_object, predict_to_julian_double(next_aos_time));
                next_aos_time -= 10;
            } while (predict_from_julian(next_aos.time) >= next_los_time);

            next_los_time = predict_from_julian(next_los.time);
            next_aos_time = predict_from_julian(next_aos.time);

            sat_next_aos_pos.az = next_aos.azimuth * RAD_TO_DEG;
            sat_next_aos_pos.el = 0;
            sat_next_los_pos.az = next_los.azimuth * RAD_TO_DEG;
            sat_next_los_pos.el = next_los.elevation * RAD_TO_DEG;

            if (true) //(is_gui)
            {
                // Calculate a few points during the pass
                predict_position satellite_orbit2;
                predict_observation observation_pos2;

                double time_step = abs(next_los_time - next_aos_time) / 50.0;

                for (double ctime = next_aos_time; ctime <= next_los_time; ctime += time_step)
                {
                    predict_orbit(satellite_object, &satellite_orbit2, predict_to_julian_double(ctime));
                    Vector3 tilt = {rotator_correction_az, rotator_correction_el, rotator_correction_el_dir};
                    predict_observe_orbit_new(tilt, satellite_observer_station, &satellite_orbit2, &observation_pos2);
                    upcoming_pass_points.push_back({float(observation_pos2.azimuth * RAD_TO_DEG), float(observation_pos2.elevation * RAD_TO_DEG)});
                }
            }
        }

        if (meridian_flip_correction)
        {
            float limitsMaxDeg = static_cast<float>(rotator_az_max);
            float limitsMinDeg = static_cast<float>(rotator_az_min);

            if(upcoming_pass_points.size() == 0) {
              upcoming_passes_mtx.unlock();
              return;
            }

            float minAzimuth = upcoming_pass_points.front().az;
            float maxAzimuth = upcoming_pass_points.back().az;
            for (auto &point : upcoming_pass_points) {
                if (point.az < minAzimuth) minAzimuth = point.az;
                if (point.az > maxAzimuth) maxAzimuth = point.az;
            }
            logger->info("Initial min-max az: (%.2f : %.2f)", minAzimuth, maxAzimuth);

            SatAzEl *prevPoint = nullptr;
            for (auto &point : upcoming_pass_points) {
                while (point.az < 0) point.az += 360.0;
                while (point.az > 360.0) point.az -= 360.0;

                if (prevPoint == nullptr) {
                    prevPoint = &point;
                    continue;
                }
                float correctedAzimutDeg = point.az;
                unsigned int numFullTurns = std::trunc(prevPoint->az / 360.0);

                float rawDeltaAngle = correctedAzimutDeg - std::fmod(prevPoint->az, 360.0);
                float deltaAngles[4], angleOffsets[4] = {0, 360.0, -360.0, -720.0};
                for (int i = 0; i < 4; i++) deltaAngles[i] = std::abs(rawDeltaAngle + angleOffsets[i]);
                unsigned int minAngleIndex = std::distance(
                        std::begin(deltaAngles),
                        std::min_element(std::begin(deltaAngles), std::end(deltaAngles))
                );
                float curDeltaAngle = angleOffsets[minAngleIndex];

                correctedAzimutDeg += curDeltaAngle + numFullTurns * 360.0;
                point.az = correctedAzimutDeg;
                prevPoint = &point;
            }

            minAzimuth = upcoming_pass_points.front().az;
            maxAzimuth = upcoming_pass_points.back().az;
            for (auto &point : upcoming_pass_points) {
                if (point.az < minAzimuth) minAzimuth = point.az;
                if (point.az > maxAzimuth) maxAzimuth = point.az;
            }
            logger->info("After gap_removal min-max az: (%.2f : %.2f)", minAzimuth, maxAzimuth);

            if ((maxAzimuth - minAzimuth) > (limitsMaxDeg - limitsMinDeg)){
              logger->error("Difference in trajectory angles is bigger than antenna limits %.2f(%.2f - %.2f) > %.2f(%.2f - %.2f)", (maxAzimuth - minAzimuth), maxAzimuth, minAzimuth, (limitsMaxDeg - limitsMinDeg), limitsMaxDeg, limitsMinDeg);
              upcoming_passes_mtx.unlock();
              return;
            }

            unsigned int numFullTurnsLeft = 0;
            if(minAzimuth > limitsMinDeg) {
              numFullTurnsLeft = std::trunc((minAzimuth - limitsMinDeg) / 360.0);
              minAzimuth -= numFullTurnsLeft * 360.0;
              maxAzimuth -= numFullTurnsLeft * 360.0;

              if(maxAzimuth > limitsMaxDeg) {
                  logger->error("Cannot reach azimuth in trajectory after turning left (%.2f : %.2f) - %d > (%.2f : %.2f)", minAzimuth, maxAzimuth, numFullTurnsLeft*360, limitsMinDeg, limitsMaxDeg);
                  upcoming_passes_mtx.unlock();
                  return;
              }
            }

            unsigned int numFullTurnsRight = 0;
            if(limitsMaxDeg > maxAzimuth) {
              numFullTurnsRight = std::trunc((limitsMaxDeg - maxAzimuth) / 360.0);
              minAzimuth += numFullTurnsRight * 360.0;
              maxAzimuth += numFullTurnsRight * 360.0;
              if(minAzimuth < limitsMinDeg){
                logger->error("Cannot reach azimuth in trajectory after turning right (%.2f : %.2f) + %d > (%.2f : %.2f)", minAzimuth, maxAzimuth, numFullTurnsRight*360, limitsMinDeg, limitsMaxDeg);
                upcoming_passes_mtx.unlock();
                return;
              }
            }

            int numFullTurns = numFullTurnsRight - numFullTurnsLeft;
            if (numFullTurns != 0){
              for (auto &point : upcoming_pass_points) {
                point.az = point.az + numFullTurns * 360.0;
              }
              logger->info("Turned whole track %d degrees. Min-max az: (%.2f : %.2f)", numFullTurns*360, minAzimuth, maxAzimuth);
            }else{
                logger->info("No additional turning needed. Min-max az: (%.2f : %.2f)", minAzimuth, maxAzimuth);
            }
            max_correction_azimuth = maxAzimuth;
            min_correction_azimuth = minAzimuth;
        }
        upcoming_passes_mtx.unlock();
    }
}
