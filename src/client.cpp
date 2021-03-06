#include "priam/client.hpp"
#include "priam/prepared.hpp"
#include "priam/result.hpp"

#include <stdexcept>
#include <utility>

using namespace std::chrono_literals;

namespace priam
{
client::client(std::unique_ptr<cluster> cluster_ptr, std::chrono::milliseconds connect_timeout)
    : m_cluster_ptr(std::move(cluster_ptr)),
      m_cass_session_ptr(cass_session_new())
{
    if (m_cass_session_ptr == nullptr)
    {
        throw std::runtime_error("Client: Failed to initialize cassandra session.");
    }

    /**
     * The Cluster aggregates hosts via "AddHost()", now that the Client owns the Cluster
     * tell the Cluster to bind all the bootstrap hosts to the cassandra cluster object.
     */
    m_cluster_ptr->bootstrap_hosts();

    auto cass_connect_future_ptr =
        cass_future_ptr(cass_session_connect(m_cass_session_ptr.get(), m_cluster_ptr->m_cass_cluster_ptr.get()));

    // Common cleanup code to free resources in the event the connection fails.
    auto cleanup = [&]() {
        m_cass_session_ptr = nullptr; // Free session + cluster, this will invoke their custom deleters.
        m_cluster_ptr      = nullptr;
    };

    // cass_future_wait_timed returns false on a timeout, so invert to get a "timed_out" bool.
    auto timed_out = !cass_future_wait_timed(
        cass_connect_future_ptr.get(),
        static_cast<cass_duration_t>(std::chrono::duration_cast<std::chrono::microseconds>(connect_timeout).count()));

    if (timed_out)
    {
        cleanup();

        std::string error_msg = "Client: Timed out attempting to connect to cassandra with timeout of: ";
        error_msg.append(std::to_string(connect_timeout.count()));
        error_msg.append(" ms.");
        throw std::runtime_error(error_msg);
    }

    // If the connect didn't time out check if there was an error.
    CassError rc = cass_future_error_code(cass_connect_future_ptr.get());
    if (rc != CassError::CASS_OK)
    {
        cleanup();

        const char* message;
        size_t      message_length;
        cass_future_error_message(cass_connect_future_ptr.get(), &message, &message_length);

        std::string error_msg{message, message_length};

        throw std::runtime_error("Client: Failed to connect to the cassandra cluster: " + error_msg);
    }

    // else Future is cleaned up via unique ptr deleter.
}

auto client::prepared_register(std::string name, std::string_view query) -> std::shared_ptr<prepared>
{
    // Using new shared_ptr as Prepared's constructor is private but friended to Client.
    auto prepared_ptr = std::shared_ptr<prepared>(new prepared(*this, query));
    m_prepared_statements.emplace(std::move(name), prepared_ptr);
    return prepared_ptr;
}

auto client::prepared_lookup(const std::string& name) -> std::shared_ptr<prepared>
{
    auto exists = m_prepared_statements.find(name);
    if (exists != m_prepared_statements.end())
    {
        return exists->second;
    }
    return {nullptr};
}

auto client::execute_statement(const statement& statement, std::chrono::milliseconds timeout, consistency c)
    -> priam::result
{
    m_active_requests.fetch_add(1, std::memory_order_relaxed);

    cass_statement_set_consistency(statement.m_cass_statement_ptr.get(), static_cast<CassConsistency>(c));
    if (timeout != 0ms)
    {
        // not really sure if this works on synchronous queries, but it can't hurt?
        cass_statement_set_request_timeout(
            statement.m_cass_statement_ptr.get(), static_cast<cass_uint64_t>(timeout.count()));
    }

    CassFuture* query_future = cass_session_execute(m_cass_session_ptr.get(), statement.m_cass_statement_ptr.get());

    if (timeout != 0ms)
    {
        // block for only as long as the timeout
        cass_future_wait_timed(
            query_future,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()));
    }
    else
    {
        // block indefinitely until the query finishes
        cass_future_wait(query_future);
    }

    // This will block until there is a response or a timeout.
    auto r = priam::result(query_future);
    m_active_requests.fetch_sub(1, std::memory_order_relaxed);
    return r;
}

struct callback_data
{
    callback_data(client& c, std::function<void(result)> on_complete_callback)
        : m_client(c),
          m_on_complete_callback(std::move(on_complete_callback))
    { }

    client& m_client;
    std::function<void(result)> m_on_complete_callback{nullptr};
};

auto client::execute_statement(
    const statement&            statement,
    std::function<void(result)> on_complete_callback,
    std::chrono::milliseconds   timeout,
    consistency                 c) -> void
{
    m_active_requests.fetch_add(1, std::memory_order_relaxed);
    auto callback_ptr = std::make_unique<callback_data>(*this, std::move(on_complete_callback));

    cass_statement_set_consistency(statement.m_cass_statement_ptr.get(), static_cast<CassConsistency>(c));

    if (timeout != 0ms)
    {
        cass_statement_set_request_timeout(
            statement.m_cass_statement_ptr.get(), static_cast<cass_uint64_t>(timeout.count()));
    }

    /**
     * The result object in the internal_on_complete_callback will take ownership of the applications
     * reference count to the query_future object.  It will 'delete' it once the result object
     * is deleted.
     *
     * Note that the underlying driver also retains a reference count to the query future and
     * deletes its reference after the internal_on_complete_callback is completed.
     */
    CassFuture* query_future = cass_session_execute(m_cass_session_ptr.get(), statement.m_cass_statement_ptr.get());

    cass_future_set_callback(query_future, internal_on_complete_callback, callback_ptr.release());
}

auto client::internal_on_complete_callback(CassFuture* query_future, void* data) -> void
{
    auto callback_data_ptr = std::unique_ptr<callback_data>(static_cast<callback_data*>(data));
    if(callback_data_ptr->m_on_complete_callback != nullptr)
    {
        callback_data_ptr->m_on_complete_callback(priam::result{query_future});
    }
    callback_data_ptr->m_client.m_active_requests.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace priam
