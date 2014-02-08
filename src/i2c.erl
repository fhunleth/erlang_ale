%%% @author Ivan Iacono <ivan.iacono@erlang-solutions.com> - Erlang Solutions Ltd
%%% @copyright (C) 2013, Erlang Solutions Ltd
%%% @doc This is the implementation of the I2C interface module.
%%% There is one process for each i2c device. Each process is linked to the supervisor
%%% of the i2c application.
%%% @end

-module(i2c).

-behaviour(gen_server).

%% API
-export([enumerate_buses/0]).
-export([start_link/1, stop/1]).
-export([write/2, read/2]).

%% gen_server callbacks
-export([init/1, handle_call/3, handle_cast/2, handle_info/2,
	 terminate/2, code_change/3]).

-define(SERVER, ?MODULE).

-record(state,
        { port         :: port(),
          pending = [] :: [term()]
        }).

-type addr() :: non_neg_integer().
-type data() :: binary().
-type len() :: non_neg_integer().
-type bus() :: non_neg_integer().
-type devname() :: string().
-type channel() :: atom().

%%%===================================================================
%%% API
%%%===================================================================

-spec enumerate_buses() -> [bus()].
enumerate_buses() ->
    {ok, Files} = file:list_dir("/dev"),
    lists:foldl(fun(Filename, A) ->
			case dev_to_bus("/dev/" ++ Filename) of
			    not_i2c -> A;
			    Bus -> [Bus | A]
			end
		end,
		[],
		Files).

%% @doc
%% Starts the process with the channel name and Initialize the devname device.
%% You can identify the device by a channel name. Each channel drive a devname device.
%% @end
-spec(start_link({channel(), bus(), addr()}) -> {ok, pid()} | {error, reason}).
start_link({Channel, Bus, Addr}) ->
    gen_server:start_link({local, Channel}, ?MODULE, {Bus, Addr}, []).

%% @doc
%% Stop the process channel and release it.
%% @end
stop(Channel) ->
    gen_server:cast(Channel, stop).

%% @doc
%% Write data into an i2c slave device.
%% @end
-spec(write(channel(), data()) -> ok | {error, reason}).
write(Channel, Data) ->
    gen_server:call(Channel, {call, write, Data}).

%% @doc
%% Read data from an i2c slave device.
%% @end
-spec(read(channel(), len()) -> {data()} | {error, reason}).
read(Channel, Len) ->
    gen_server:call(Channel, {call, read, Len}).

%%%===================================================================
%%% gen_server callbacks
%%%===================================================================

%%--------------------------------------------------------------------
%% @private
%% @doc
%% Initializes the server
%%
%% @spec init(Args) -> {ok, State} |
%%                     {ok, State, Timeout} |
%%                     ignore |
%%                     {stop, Reason}
%% @end
%%--------------------------------------------------------------------
init({Bus, Addr}) ->
    Devname = bus_to_dev(Bus),
    Executable = code:priv_dir(erlang_ale) ++ "/i2c_port",
    Port = open_port({spawn_executable, Executable},
		     [{args, [Devname, integer_to_list(Addr)]},
		      {packet, 2},
		      exit_status,
		      binary]),
    {ok, #state{port=Port}}.

%%--------------------------------------------------------------------
%% @private
%% @doc
%% Handling call messages
%%
%% @spec handle_call(Request, From, State) ->
%%                                   {reply, Reply, State} |
%%                                   {reply, Reply, State, Timeout} |
%%                                   {noreply, State} |
%%                                   {noreply, State, Timeout} |
%%                                   {stop, Reason, Reply, State} |
%%                                   {stop, Reason, State}
%% @end
%%--------------------------------------------------------------------
handle_call({call, write, Data}, _From, #state{port=Port} = State) ->
    Len = byte_size(Data),
    case port_lib:sync_call_to_port(Port, {i2c_write, Data, Len}) < 0 of
	true ->
	    Reply = {error, i2c_write_error};
	false ->
	    Reply = ok
    end,
    {reply, Reply, State};

handle_call({call, read, Len}, _From, #state{port=Port} = State) ->
    case port_lib:sync_call_to_port(Port, {i2c_read, Len}) < 0 of
	true ->
	    Reply = {error, i2c_read_error};
	false ->
	    Reply = ok
    end,
    {reply, Reply, State}.


%%--------------------------------------------------------------------
%% @private
%% @doc
%% Handling cast messages
%%
%% @spec handle_cast(Msg, State) -> {noreply, State} |
%%                                  {noreply, State, Timeout} |
%%                                  {stop, Reason, State}
%% @end
%%--------------------------------------------------------------------

handle_cast(stop, State) ->
    {stop, normal, State}.

%%--------------------------------------------------------------------
%% @private
%% @doc
%% Handling all non call/cast messages
%%
%% @spec handle_info(Info, State) -> {noreply, State} |
%%                                   {noreply, State, Timeout} |
%%                                   {stop, Reason, State}
%% @end
%%--------------------------------------------------------------------
handle_info({Port, {exit_status, _Status}}, #state{port=Port}=State) ->
    {stop, port_crashed, State}.

%%--------------------------------------------------------------------
%% @private
%% @doc
%% This function is called by a gen_server when it is about to
%% terminate. It should be the opposite of Module:init/1 and do any
%% necessary cleaning up. When it returns, the gen_server terminates
%% with Reason. The return value is ignored.
%%
%% @spec terminate(Reason, State) -> void()
%% @end
%%--------------------------------------------------------------------
terminate(_Reason, _State) ->
    ok.

%%--------------------------------------------------------------------
%% @private
%% @doc
%% Convert process state when code is changed
%%
%% @spec code_change(OldVsn, State, Extra) -> {ok, NewState}
%% @end
%%--------------------------------------------------------------------
code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

%%%===================================================================
%%% Internal functions
%%%===================================================================
-spec bus_to_dev(bus()) -> devname().
bus_to_dev(Bus) ->
    "/dev/i2c-" ++ integer_to_list(Bus).

-spec dev_to_bus(devname()) -> not_i2c | bus().
dev_to_bus(Dev) ->
    try
	{"/dev/i2c-", BusString} = lists:split(9, Dev),
	list_to_integer(BusString)
    catch
	error:_ -> not_i2c
    end.
