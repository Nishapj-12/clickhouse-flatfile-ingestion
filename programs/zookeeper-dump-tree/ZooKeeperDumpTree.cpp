#include <list>
#include <iostream>
#include <boost/program_options.hpp>
#include <Common/Exception.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/KeeperException.h>


int mainEntryClickHouseZooKeeperDumpTree(int argc, char ** argv)
{
    try
    {
        boost::program_options::options_description desc("Allowed options");
        desc.add_options()
            ("help,h", "produce help message")
            ("address,a", boost::program_options::value<std::string>()->required(),
                "addresses of ZooKeeper instances, comma separated. Example: example01e.clickhouse.com:2181")
            ("path,p", boost::program_options::value<std::string>()->default_value("/"),
                "where to start")
            ("ctime,c", "print node ctime")
        ;

        boost::program_options::variables_map options;
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), options);

        if (options.count("help"))
        {
            std::cout << "Dump paths of all nodes in ZooKeeper." << std::endl;
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << desc << std::endl;
            return 1;
        }

        bool dump_ctime = options.count("ctime");

        zkutil::ZooKeeperPtr zookeeper = zkutil::ZooKeeper::createWithoutKillingPreviousSessions(options.at("address").as<std::string>());

        std::string initial_path = options.at("path").as<std::string>();

        std::list<std::pair<std::string, std::future<Coordination::ListResponse>>> list_futures;
        list_futures.emplace_back(initial_path, zookeeper->asyncGetChildren(initial_path));

        size_t num_reconnects = 0;
        constexpr size_t max_reconnects = 100;

        auto ensure_session = [&]
        {
            if (zookeeper->expired())
            {
                if (num_reconnects == max_reconnects)
                    return false;
                ++num_reconnects;
                std::cerr << "num_reconnects: " << num_reconnects << "\n";
                zookeeper = zookeeper->startNewSession();
            }
            return true;
        };

        for (auto it = list_futures.begin(); it != list_futures.end(); ++it)
        {
            Coordination::ListResponse response;

            try
            {
                response = it->second.get();
            }
            catch (const Coordination::Exception & e)
            {
                if (e.code == Coordination::Error::ZNONODE)
                {
                    continue;
                }
                if (Coordination::isHardwareError(e.code))
                {
                    /// Reinitialize the session and move the node to the end of the queue for later retry.
                    if (!ensure_session())
                        throw;
                    list_futures.emplace_back(it->first, zookeeper->asyncGetChildren(it->first));
                    continue;
                }

                throw;
            }

            std::cout << it->first << '\t' << response.stat.numChildren << '\t' << response.stat.dataLength;
            if (dump_ctime)
                std::cout << '\t' << response.stat.ctime;
            std::cout << '\n';

            for (const auto & name : response.names)
            {
                std::string child_path = it->first == "/" ? it->first + name : it->first + '/' + name;

                ensure_session();
                list_futures.emplace_back(child_path, zookeeper->asyncGetChildren(child_path));
            }
        }

        return 0;
    }
    catch (...)
    {
        std::cerr << DB::getCurrentExceptionMessage(true) << '\n';
        return 1;
    }
}
