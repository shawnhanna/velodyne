/*
 *  Copyright (C) 2007 Austin Robot Technology, Patrick Beeson
 *  Copyright (C) 2009, 2010 Austin Robot Technology, Jack O'Quin
 *  Copyright (C) 2015, Jack O'Quin
 *  Modified 2018 Kaarta - Shawn Hanna
 *  License: Modified BSD Software License Agreement
 *
 *  $Id$
 */

/** \file
 *
 *  Input classes for the Velodyne HDL-64E 3D LIDAR:
 *
 *     Input -- base class used to access the data independently of
 *              its source
 *
 *     InputSocket -- derived class reads live data from the device
 *              via a UDP socket
 *
 *     InputPCAP -- derived class provides a similar interface from a
 *              PCAP dump
 */

#include <unistd.h>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <velodyne_driver/input.h>
#include "boost/date_time/posix_time/posix_time.hpp"

namespace velodyne_driver
{
  // from https://www.winpcap.org/docs/docs_412/html/group__wpcap__tut6.html
  /* 4 bytes IP address */
  typedef struct ip_address{
      u_char byte1;
      u_char byte2;
      u_char byte3;
      u_char byte4;
  }ip_address;

  /* IPv4 header */
  typedef struct ip_header{
      u_char  ver_ihl;        // Version (4 bits) + Internet header length (4 bits)
      u_char  tos;            // Type of service 
      u_short tlen;           // Total length 
      u_short identification; // Identification
      u_short flags_fo;       // Flags (3 bits) + Fragment offset (13 bits)
      u_char  ttl;            // Time to live
      u_char  proto;          // Protocol
      u_short crc;            // Header checksum
      ip_address  saddr;      // Source address
      ip_address  daddr;      // Destination address
      u_int   op_pad;         // Option + Padding
  }ip_header;

  /* UDP header*/
  typedef struct udp_header{
      u_short sport;          // Source port
      u_short dport;          // Destination port
      u_short len;            // Datagram length
      u_short crc;            // Checksum
  }udp_header;

  static const size_t packet_size =
    sizeof(velodyne_msgs::VelodynePacket().data);

  ////////////////////////////////////////////////////////////////////////
  // Input base class implementation
  ////////////////////////////////////////////////////////////////////////

  /** @brief constructor
   *
   *  @param private_nh ROS private handle for calling node.
   *  @param port UDP port number.
   *  @param position_port. set to 0 to disable position packet processing
   */
  Input::Input(ros::NodeHandle private_nh, uint16_t port, uint16_t position_port) :
    private_nh_(private_nh),
    port_(port),
    position_port_(position_port),
    process_position_packets_(position_port != 0),
    pps_status_(0)
  {
    private_nh.param("device_ip", devip_str_, std::string(""));
    if (!devip_str_.empty())
      ROS_INFO_STREAM("Only accepting packets from IP address: "
                      << devip_str_);
  }

  velodyne_msgs::VelodynePositionPacket Input::getPositionPacket(){
    std::unique_lock<std::mutex> lock(position_data_mutex_);
    auto ret = last_position_packet_;
    last_position_packet_.stamp.fromSec(0);
    // last_position_packet_.data.fill(0);
    return ret;
  }

  uint8_t Input::getPPSStatus(){
    std::unique_lock<std::mutex> lock(position_data_mutex_);
    return pps_status_;
  }

  ////////////////////////////////////////////////////////////////////////
  // InputSocket class implementation
  ////////////////////////////////////////////////////////////////////////

  /** @brief constructor
   *
   *  @param private_nh ROS private handle for calling node.
   *  @param port UDP port number for velodyne point data packet
   *  @param position_port UDP port number for position packet
   */
  InputSocket::InputSocket(ros::NodeHandle private_nh, uint16_t port, uint16_t position_port):
    Input(private_nh, port, position_port)
  {
    position_sockfd_ = sockfd_ = -1;
    
    if (!devip_str_.empty()) {
      inet_aton(devip_str_.c_str(),&devip_);
    }

    // connect to Velodyne UDP port
    ROS_INFO_STREAM("Opening UDP socket: port " << port);
    sockfd_ = socket(PF_INET, SOCK_DGRAM, 0);
    if (sockfd_ == -1)
      {
        ROS_ERROR("Error connecting to Velodyne");
        perror("socket");               // TODO: ROS_ERROR errno
        return;
      }
  
    sockaddr_in my_addr;                     // my address information
    memset(&my_addr, 0, sizeof(my_addr));    // initialize to zeros
    my_addr.sin_family = AF_INET;            // host byte order
    my_addr.sin_port = htons(port);          // port in network byte order
    my_addr.sin_addr.s_addr = INADDR_ANY;    // automatically fill in my IP
  
    if (bind(sockfd_, (sockaddr *)&my_addr, sizeof(sockaddr)) == -1)
      {
        ROS_ERROR("Error binding socket for Velodyne position data");
        perror("bind");                 // TODO: ROS_ERROR errno
        return;
      }
  
    if (fcntl(sockfd_,F_SETFL, O_NONBLOCK|FASYNC) < 0)
      {
        ROS_ERROR("Error setting up non-blocking socket for Velodyne position data");
        perror("non-block");
        return;
      }

    ROS_DEBUG("Velodyne socket fd is %d\n", sockfd_);

    if (process_position_packets_){
      ROS_INFO_STREAM("Opening UDP socket: position port " << position_port_);

      position_sockfd_ = socket(PF_INET, SOCK_DGRAM, 0);

      if (position_sockfd_ == -1)
      {
        ROS_ERROR("Error connecting to Velodyne position packet");
        perror("socket");               // TODO: ROS_ERROR errno
        return;
      }

      sockaddr_in my_addr;                     // my address information
      memset(&my_addr, 0, sizeof(my_addr));    // initialize to zeros
      my_addr.sin_family = AF_INET;            // host byte order
      my_addr.sin_port = htons(position_port_);          // port in network byte order
      my_addr.sin_addr.s_addr = INADDR_ANY;    // automatically fill in my IP=
      
      if (bind(position_sockfd_, (sockaddr *)&my_addr, sizeof(sockaddr)) == -1)
        {
          ROS_ERROR("Error binding socket for Velodyne position data");
          perror("bind");                 // TODO: ROS_ERROR errno
          return;
        }
    
      if (fcntl(position_sockfd_,F_SETFL, O_NONBLOCK|FASYNC) < 0)
        {
          ROS_ERROR("Error setting up non-blocking socket for Velodyne position data");
          perror("non-block");
          return;
        }

      ROS_DEBUG("Velodyne position socket fd is %d\n", position_sockfd_);
    }
  }

  /** @brief destructor */
  InputSocket::~InputSocket(void)
  {
    (void) close(sockfd_);
  }

  /** @brief Get one velodyne packet. */
  int InputSocket::getPacket(velodyne_msgs::VelodynePacket *pkt, const double time_offset)
  {
    // double time1 = ros::Time::now().toSec();

    struct pollfd fds[1];
    fds[0].fd = sockfd_;
    fds[0].events = POLLIN;
    static const int POLL_TIMEOUT = 1000; // one second (in msec)

    sockaddr_in sender_address;
    socklen_t sender_address_len = sizeof(sender_address);

    while (true)
      {
        // Unfortunately, the Linux kernel recvfrom() implementation
        // uses a non-interruptible sleep() when waiting for data,
        // which would cause this method to hang if the device is not
        // providing data.  We poll() the device first to make sure
        // the recvfrom() will not block.
        //
        // Note, however, that there is a known Linux kernel bug:
        //
        //   Under Linux, select() may report a socket file descriptor
        //   as "ready for reading", while nevertheless a subsequent
        //   read blocks.  This could for example happen when data has
        //   arrived but upon examination has wrong checksum and is
        //   discarded.  There may be other circumstances in which a
        //   file descriptor is spuriously reported as ready.  Thus it
        //   may be safer to use O_NONBLOCK on sockets that should not
        //   block.

        // poll() until input available
        do
          {
            int retval = poll(fds, 1, POLL_TIMEOUT);
            if (retval < 0)             // poll() error?
              {
                if (errno != EINTR)
                  ROS_ERROR("poll() error: %s", strerror(errno));
                return 1;
              }
            if (retval == 0)            // poll() timeout?
              {
                ROS_WARN_THROTTLE(60, "Velodyne poll() timeout");
                return 1;
              }
            if ((fds[0].revents & POLLERR)
                || (fds[0].revents & POLLHUP)
                || (fds[0].revents & POLLNVAL)) // device error?
              {
                ROS_ERROR("poll() reports Velodyne error");
                return 1;
              }
          } while ((fds[0].revents & POLLIN) == 0);
          
        // Receive packets that should now be available from the
        // socket using a blocking read.
        ssize_t nbytes = recvfrom(sockfd_, &pkt->data[0],
                                  packet_size,  0,
                                  (sockaddr*) &sender_address,
                                  &sender_address_len);
        pkt->stamp = parseInternalTime(&pkt->data[0] + 1200, ros::Time::now());

        if (nbytes < 0)
          {
            if (errno != EWOULDBLOCK)
              {
                perror("recvfail");
                ROS_INFO("recvfail");
                return 1;
              }
          }
        else if ((size_t) nbytes == packet_size)
          {
            // read successful,
            // if packet is not from the lidar scanner we selected by IP,
            // continue otherwise we are done
            if(devip_str_ != ""
               && sender_address.sin_addr.s_addr != devip_.s_addr)
              continue;
            else
              break; //done
          }

        ROS_DEBUG_STREAM("incomplete Velodyne packet read: "
                         << nbytes << " bytes");
      }

    return 0;
  }

  /** @brief Get one velodyne packet. */
  int InputSocket::getPositionPacket(velodyne_msgs::VelodynePositionPacket *pkt, const double time_offset)
  {
    // double time1 = ros::Time::now().toSec();

    struct pollfd fds[1];
    fds[0].fd = position_sockfd_;
    fds[0].events = POLLIN;
    static const int POLL_TIMEOUT = 1000; // one second (in msec)

    sockaddr_in sender_address;
    socklen_t sender_address_len = sizeof(sender_address);

    while (true)
      {
        // Unfortunately, the Linux kernel recvfrom() implementation
        // uses a non-interruptible sleep() when waiting for data,
        // which would cause this method to hang if the device is not
        // providing data.  We poll() the device first to make sure
        // the recvfrom() will not block.
        //
        // Note, however, that there is a known Linux kernel bug:
        //
        //   Under Linux, select() may report a socket file descriptor
        //   as "ready for reading", while nevertheless a subsequent
        //   read blocks.  This could for example happen when data has
        //   arrived but upon examination has wrong checksum and is
        //   discarded.  There may be other circumstances in which a
        //   file descriptor is spuriously reported as ready.  Thus it
        //   may be safer to use O_NONBLOCK on sockets that should not
        //   block.

        // poll() until input available
        do
          {
            int retval = poll(fds, 1, POLL_TIMEOUT);
            if (retval < 0)             // poll() error?
              {
                if (errno != EINTR)
                  ROS_ERROR("poll() error: %s", strerror(errno));
                return -1;
              }
            if (retval == 0)            // poll() timeout?
              {
                ROS_WARN_THROTTLE(16, "Velodyne position poll() timeout");
                return -1;
              }
            if ((fds[0].revents & POLLERR)
                || (fds[0].revents & POLLHUP)
                || (fds[0].revents & POLLNVAL)) // device error?
              {
                ROS_ERROR("position poll() reports Velodyne error");
                return -1;
              }
          } while ((fds[0].revents & POLLIN) == 0);
          
        // Receive packets that should now be available from the
        // socket using a blocking read.
        static const size_t position_packet_size = sizeof(velodyne_msgs::VelodynePositionPacket::data);
        ssize_t nbytes = recvfrom(position_sockfd_, &pkt->data[0],
                                  position_packet_size,  0,
                                  (sockaddr*) &sender_address,
                                  &sender_address_len);

        // ROS_WARN("%x %x %x %x - %x", pkt->data[0x00F0-42],pkt->data[0x00F1-42],pkt->data[0x00F2-42],pkt->data[0x00F3-42],pkt->data[0x00F4-42]);

        if (nbytes < 0)
          {
            if (errno != EWOULDBLOCK)
              {
                perror("recvfail");
                ROS_INFO("recvfail");
                return -1;
              }
          }
        else if ((size_t) nbytes == position_packet_size)
          {
            // read successful,
            // if packet is not from the lidar scanner we selected by IP,
            // continue otherwise we are done
            if(devip_str_ != ""
               && sender_address.sin_addr.s_addr != devip_.s_addr)
              continue;
            else{
              pkt->stamp = parseInternalTime(&pkt->data[0] + 0xC6, ros::Time::now());
              std::unique_lock<std::mutex> lock(position_data_mutex_);
              pps_status_ = *((const uint8_t*)(&(pkt->data[0]) + 0xCA));

              last_position_packet_ = *pkt;
              break; //done
            }
          }
          else{
            ROS_ERROR_THROTTLE(1, "Getting packets that aren't the right size");
          }

        ROS_WARN_STREAM("incomplete Velodyne position packet read: "
                         << nbytes << " bytes");
      }

    // Average the times at which we begin and end reading.  Use that to
    // estimate when the scan occurred. Add the time offset.
    // double time2 = ros::Time::now().toSec();
    // ros::Time t3 = ros::Time((time2 + time1) / 2.0);
    // ROS_INFO_STREAM_THROTTLE(1, "Stamp: " << pkt->stamp << " realtime : " << t3);

    return 0;
  }

  ////////////////////////////////////////////////////////////////////////
  // InputPCAP class implementation
  ////////////////////////////////////////////////////////////////////////

  /** @brief constructor
   *
   *  @param private_nh ROS private handle for calling node.
   *  @param port UDP port number
   *  @param packet_rate expected device packet frequency (Hz)
   *  @param filename PCAP dump file name
   */
  InputPCAP::InputPCAP(ros::NodeHandle private_nh, uint16_t port, uint16_t position_port,
                       std::string filename,
                       bool read_once, bool read_fast, double repeat_delay):
    Input(private_nh, port, position_port),
    new_position_packet_(false),
    filename_(filename)
  {
    pcap_ = NULL;  
    empty_ = true;

    // get parameters using private node handle
    private_nh.param("read_once", read_once_, false);
    private_nh.param("read_fast", read_fast_, false);
    private_nh.param("repeat_delay", repeat_delay_, 0.0);

    if (read_once_)
      ROS_INFO("Read input file only once.");
    if (read_fast_)
      ROS_INFO("Read input file as quickly as possible.");
    if (repeat_delay_ > 0.0)
      ROS_INFO("Delay %.3f seconds before repeating input file.",
               repeat_delay_);

    // Open the PCAP dump file
    ROS_INFO("Opening PCAP file \"%s\"", filename_.c_str());
    if ((pcap_ = pcap_open_offline(filename_.c_str(), errbuf_) ) == NULL)
    {
      ROS_FATAL("Error opening Velodyne socket dump file.");
      return;
    }

    std::stringstream filter;
    if( devip_str_ != "" )              // using specific IP?
    {
      filter << "src host " << devip_str_ << " && ";
    }

    if(process_position_packets_)
    {
      filter << "udp dst port (" << port_ << " || " << position_port_ << ")";
    }
    else{
      filter << "udp dst port " << port_;
    }
    pcap_compile(pcap_, &pcap_packet_filter_,
                 filter.str().c_str(), 1, PCAP_NETMASK_UNKNOWN);
  }

  /** destructor */
  InputPCAP::~InputPCAP(void)
  {
    pcap_close(pcap_);
  }

  /** @brief Get one velodyne packet. */
  int InputPCAP::getPacket(velodyne_msgs::VelodynePacket *pkt, const double time_offset)
  {
    struct pcap_pkthdr *header;
    const u_char *pkt_data;

    while (true)
      {
        int res;
        if ((res = pcap_next_ex(pcap_, &header, &pkt_data)) >= 0)
          {
            // Skip packets not for the correct port and from the
            // selected IP address.
            if (0 == pcap_offline_filter(&pcap_packet_filter_,
                                          header, pkt_data))
              continue;

            /* retireve the position of the ip header */
            ip_header *ih;
            udp_header *uh;
            u_int ip_len;
            u_short sport/* ,dport */;
            ih = (ip_header *) (pkt_data + 14); //length of ethernet header

            /* retireve the position of the udp header */
            ip_len = (ih->ver_ihl & 0xf) * 4;
            uh = (udp_header *) ((u_char*)ih + ip_len);

            /* convert from network byte order to host byte order */
            sport = ntohs( uh->sport );
            // dport = ntohs( uh->dport );

            if (process_position_packets_){
              if (sport == position_port_){
                // velodyne position data

                // if we got new data, don't send
                // if (0 != memcmp(last_position_packet_.data.data(), pkt_data+42, sizeof(velodyne_msgs::VelodynePositionPacket().data)))
                {
                  std::lock_guard<std::mutex> lock(position_data_mutex_);
                  memcpy(last_position_packet_.data.data(), pkt_data+42, sizeof(velodyne_msgs::VelodynePositionPacket().data));
                  last_position_packet_.stamp = parseInternalTime( (uint8_t*)&last_position_packet_.data[0]+0xC6,
                                                                  ros::Time(header->ts.tv_sec, header->ts.tv_usec));

                  pps_status_ = *((const uint8_t*)(&last_position_packet_.data[0]+0xCA));

                  new_position_packet_ = true;
                }
                position_data_conditional_variable_.notify_one();

                // get the next velodyne point ready. This function is meant to get the next velodyne point
                continue;
              }
            }
            if (sport == port_){
              // velodyne point data
              memcpy(&pkt->data[0], pkt_data+42, packet_size);
              pkt->stamp = parseInternalTime((uint8_t*)pkt_data + 1242, ros::Time(header->ts.tv_sec, header->ts.tv_usec)); // time_offset not considered here, as no synchronization required

              // ROS_INFO_STREAM("Got data packet time: "<<pkt->stamp);
            }

            // Keep the reader from blowing through the file.
            if (read_fast_ == false)
            {
              ros::Time packet_time(header->ts.tv_sec, header->ts.tv_usec * 1000);
              // ROS_INFO_STREAM("Got data on port: " << dport);

              if (!last_time_.isZero())
                (packet_time-last_time_).sleep();

              last_time_ = packet_time;
            }

            empty_ = false;
            return 0;                   // success
          }

        if (empty_)                 // no data in file?
          {
            ROS_WARN("Error %d reading Velodyne packet: %s", 
                     res, pcap_geterr(pcap_));
            return -1;
          }

        if (read_once_)
          {
            ROS_INFO("end of file reached -- done reading.");
            return -1;
          } 
        
        if (repeat_delay_ > 0.0)
          {
            ROS_INFO("end of file reached -- delaying %.3f seconds.",
                     repeat_delay_);
            usleep(rint(repeat_delay_ * 1000000.0));
          }

        ROS_DEBUG("replaying Velodyne dump file");

        // I can't figure out how to rewind the file, because it
        // starts with some kind of header.  So, close the file
        // and reopen it with pcap.
        pcap_close(pcap_);
        pcap_ = pcap_open_offline(filename_.c_str(), errbuf_);
        empty_ = true;              // maybe the file disappeared?
      } // loop back and try again
  }

  /** @brief Get one velodyne packet. */
  int InputPCAP::getPositionPacket(velodyne_msgs::VelodynePositionPacket *pkt, const double time_offset)
  {
    if (process_position_packets_ == false){
      return -1;
    }
    // Wait until main() sends data
    std::unique_lock<std::mutex> lk(position_data_mutex_);
    position_data_conditional_variable_.wait(lk, [&]{return !ros::ok() || new_position_packet_;});

    if (!ros::ok() || new_position_packet_ == false){
      return 1;
    }

    *pkt = last_position_packet_;
    pkt->stamp = pkt->stamp + ros::Duration(time_offset);
    new_position_packet_ = false;
    return 0;
  }

  ros::Time Input::parseInternalTime(const uint8_t* bytes, const ros::Time& system_time) const
  {
    uint32_t usec = (*(uint32_t*)bytes);

    double sensorSec = usec / 1000000.0;
    double computerTime = system_time.toSec();
    double sensorTime = 3600 * int(computerTime / 3600.0) + sensorSec;

    if (sensorTime - computerTime > 1800) sensorTime -= 3600;
    else if (sensorTime - computerTime < -1800) sensorTime += 3600;
    
    return ros::Time(sensorTime);
  }
} // velodyne namespace
