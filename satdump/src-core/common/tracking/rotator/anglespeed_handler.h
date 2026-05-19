#pragma once

#include <boost/asio.hpp>
#include <boost/asio/serial_port.hpp>
#include <atomic>
#include <thread>
#include <mutex>
#include <regex>
#include "rotator_handler.h"

namespace rotator {
  class AngleSpeedHandler : public RotatorHandler {
  private:
    boost::asio::io_service io_service;
    std::unique_ptr<boost::asio::io_service::work> work;
    boost::asio::serial_port serial;
    std::thread io_thread;
    std::atomic<bool> running;
    unsigned int seq_ = 0;

    static const size_t max_buffer_size = 1024;
    boost::asio::streambuf read_buffer;
    std::mutex data_mutex;

    float current_azimuth = 0;
    float current_elevation = 0;

    int baud_rate = 115200;

    char input_address[100] = "/dev/ttyr00";
  private:
    void process_serial_data(const boost::system::error_code& error, size_t bytes_transferred);

    void process_packet(const std::smatch &matches);

    void process_azimuth(uint16_t state_ctrl, const std::string &az_str);

    void process_elevation(uint16_t state_ctrl, const std::string &el_str);

    void start_read();

    void l_connect(char *address, int port);

    void l_disconnect();

  public:
    AngleSpeedHandler();

    ~AngleSpeedHandler();

    std::string get_id();

    void set_settings(nlohmann::json settings);

    nlohmann::json get_settings();

    rotator_status_t get_pos(float *az, float *el);

    rotator_status_t set_pos(float az, float el, float az_vel = 0, float el_vel = 0);

    void render();

    bool is_connected();

    void connect();

    void disconnect();
  };
}