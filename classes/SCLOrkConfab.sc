SCLOrkConfab {
	classvar confabPid;
	classvar addSerial = 1;

	classvar confab;
	classvar assetAddedFunc;
	classvar assetErrorFunc;
	classvar assetFoundFunc;
	classvar assetLoadedFunc;
	classvar listFoundFunc;
	classvar listErrorFunc;
	classvar listItemsFunc;
	classvar statesFunc;

	classvar addCallbackMap;
	classvar findCallbackMap;
	classvar loadCallbackMap;
	classvar listCallbackMap;
	classvar statesCallbackQueue;

	*start { |
		confabBindPort = 4248,
		scBindPort = 4249,
		pathToConfabBinary = nil,
		pathToConfabDataDir = nil |

		confab = NetAddr.new("127.0.0.1", confabBindPort);
		addCallbackMap = IdentityDictionary.new;
		findCallbackMap = IdentityDictionary.new;
		loadCallbackMap = IdentityDictionary.new;
		listCallbackMap = IdentityDictionary.new;
		statesCallbackQueue = List.new;

		SCLOrkConfab.prBindResponseMessages(scBindPort);
		SCLOrkConfab.prStartConfab;
	}

	*addAssetFile { |type, name, lists, filePath, addCallback|
		addCallbackMap.put(addSerial, addCallback);
		confab.sendMsg('/assetAddFile', addSerial, type, name, lists, filePath);
		addSerial = addSerial + 1;
	}

	*addAssetString { |type, name, lists, assetString, addCallback|
		addCallbackMap.put(addSerial, addCallback);
		confab.sendMsg('/assetAddString', addSerial, type, name, lists, assetString);
		addSerial = addSerial + 1;
	}

	*findAssetById { |id, callback|
		findCallbackMap.put(id, callback);
		confab.sendMsg('/assetFind', id);
	}

	*findAssetByName { |name, callback|
		findCallbackMap.put(name, callback);
		confab.sendMsg('/assetFindName', name);
	}

	*loadAssetById { |id, callback|
		loadCallbackMap.put(id, callback);
		confab.sendMsg('/assetLoad', id);
	}

	*createList { |name, callback|
		listCallbackMap.put(name, callback);
		confab.sendMsg('/listAdd', name);
	}

	*findListByName { |name, callback|
		listCallbackMap.put(name, callback);
		confab.sendMsg('/listFind', name);
	}

	*getListNext { |listId, fromToken, callback|
		listCallbackMap.put(listId, callback);
		confab.sendMsg('/listNext', listId, fromToken);
	}

	*getStates { |callback|
		// States callbacks are handled first-come-first-serve.
		statesCallbackQueue.add(callback);
		confab.sendMsg('/state');
	}

	*setUser { |userId|
		confab.sendMsg('/setUser', userId);
	}

	*isConfabRunning {
		if (confabPid.notNil, {
			^confabPid.pidRunning;
		});
		^false;
	}

	*idValid { |id|
		^(
			id.class.asSymbol === 'Symbol'
			and: { id !== '0000000000000000' }
			and: { id !== 'ffffffffffffffff' }
		);
	}

	*prBindResponseMessages { | recvPort |
		assetAddedFunc = OSCFunc.new({ | msg, time, addr |
			var serial = msg[1];
			var key = msg[2];
			var callback = addCallbackMap.at(serial);
			if (callback.notNil, {
				addCallbackMap.removeAt(serial);
				callback.value(key.asSymbol);
			}, {
				"confab got add callback on missing serial %".format(serial).postln;
			});
		},
		'/assetAdded',
		recvPort: recvPort);

		assetErrorFunc = OSCFunc.new({ |msg, time, addr|
			var requestedKey = msg[1];
			var errorMessage = msg[2];
			var callback = findCallbackMap.at(requestedKey);

			"asset error: %".format(errorMessage).postln;

			if (callback.notNil, {
				callback.value(requestedKey, nil);
				findCallbackMap.removeAt(requestedKey);
			}, {
				"confab got error callback on missing Asset id %".format(requestedKey).postln;
			});
		},
		'/assetError',
		recvPort: recvPort);

		assetFoundFunc = OSCFunc.new({ |msg, time, addr|
			var requestedKey = msg[1];
			var returnedKey = msg[2];
			var assetType = msg[3];
			var name = msg[4];
			var inlineData = msg[8];

			var asset = SCLOrkAsset.newFromArgs(returnedKey, assetType, name, inlineData);

			var callback = findCallbackMap.at(requestedKey);

			if (callback.notNil, {
				callback.value(requestedKey, asset);
				findCallbackMap.removeAt(requestedKey);
			}, {
				"confab got found callback on missing Asset id %".format(requestedKey).postln;
			});
		},
		'/assetFound',
		recvPort: recvPort);

		assetLoadedFunc = OSCFunc.new({ |msg, time, addr|
			var requestedKey = msg[1];
			var returnedKey = msg[2];
			var assetPath = msg[3];

			var callback = loadCallbackMap.at(requestedKey);

			if (callback.notNil, {
				callback.value(requestedKey, assetPath);
				loadCallbackMap.removeAt(requestedKey);
			}, {
				"confab got loaded callback on missing Asset id %".format(requestedKey).postln;
			});
		},
		'/assetLoaded',
		recvPort: recvPort);

		listFoundFunc = OSCFunc.new({ |msg, time, addr|
			var listName = msg[1];
			var listKey = msg[2];
			var callback = listCallbackMap.at(listName);
			if (callback.notNil, {
				callback.value(listName, listKey);
				listCallbackMap.removeAt(listName);
			}, {
				"confab /listFound got list callback on missing list name %".format(listName).postln;
			});
		},
		'/listFound',
		recvPort: recvPort);

		listErrorFunc = OSCFunc.new({ |msg, time, addr|
			var listName = msg[1]; // TODO: could be a Name *or* a key right now.
			var listKey = msg[2];
			var callback = listCallbackMap.at(listName);
			if (callback.notNil, {
				callback.value(listName, listKey);
			}, {
				"confab got list callback on missing item %".format(listName).postln;
			});
		},
		'/listError',
		recvPort: recvPort);

		listItemsFunc = OSCFunc.new({ |msg, time, addr|
			var listId = msg[1];
			var tokens = msg[2];
			var callback = listCallbackMap.at(listId);
			if (callback.notNil, {
				callback.value(listId, tokens);
			}, {
				"confab listnext got callback on missing item %".format(listId).postln;
			});
		},
		'/listItems',
		recvPort: recvPort);

		statesFunc = OSCFunc.new({ |msg, time, addr|
			var states = msg[1..].asDict;
			var callback = statesCallbackQueue.pop();
			callback.value(states);
		},
		'/states',
		recvPort: recvPort);
	}

	*prStartConfab { |
		confabBindPort = 4248,
		scBindPort = 4249,
		pathToConfabBinary = nil,
		pathToConfabDataDir = nil |
		var command;
		// Check if confab binary already running.
		if (SCLOrkConfab.isConfabRunning, { ^true; });

		// Construct default path to binary if path not provided.
		if (pathToConfabBinary.isNil or: { pathToConfabDataDir.isNil }, {
			var quarkPath;
			// Quarks.quarkNameAsLocalPath seems to fail if the Quark is installed manually, so
			// instead we search the list of installed Quarks for the directory that refers to
			// SCLOrkTools.
			Quarks.installedPaths.do({ | path, index |
				if (path.contains("SCLOrkTools"), {
					quarkPath = path;
				});
			});

			if (pathToConfabBinary.isNil, {
				pathToConfabBinary = quarkPath ++ "/build/src/confab/confab";
			});
			if (pathToConfabDataDir.isNil, {
				pathToConfabDataDir = quarkPath ++ "/data/confab";
			});
		});

		// Check if a confab process is already running by checking for pid sentinel file.
		if (File.exists(pathToConfabDataDir ++ "/pid"), {
			confabPid = File.readAllString(pathToConfabDataDir ++ "/pid").asInteger;
			^confabPid.pidRunning;
		});

		// For now we assume both that the binary and database exist.
		command = [
			pathToConfabBinary,
			"--chatty=true",
			"--data_directory=" ++ pathToConfabDataDir
		];
		command.postln;
		confabPid = command.unixCmd({ | exitCode, exitPid |
			SCLOrkConfab.prOnConfabExit(exitCode, exitPid)
		});

		^confabPid.pidRunning;
	}


	*prStopConfab {
		if (confabPid.notNil, {
			["kill", "-SIGINT", confabPid.asString].unixCmd;
			confabPid = nil;
		});
	}

	*prOnConfabExit { | exitCode, exitPid |
		if (exitCode != 0, {
			"*** confab abnormal exit, pid: % exit: %".format(exitPid, exitCode).postln;
		});
		confabPid = nil;
	}
}