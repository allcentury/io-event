# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'io/event'
require 'io/event/selector'
require 'socket'

ThreadSafeSelector = Sus::Shared("thread-safe selector") do
	let(:pipe) {IO.pipe}
	let(:input) {pipe.first}
	let(:output) {pipe.last}
	
	with '#io_write' do
		it "it can get interrupted" do
			mutex = Thread::Mutex.new
			condition = Thread::ConditionVariable.new
			q = 0.001
			repeats = 1000
			
			thread = Thread.new do
				while true
					mutex.synchronize do
						$stderr.puts "."
						condition.signal
					end
					sleep q
				end
			end
			
			i = 0
			output.sync = true
			
			fiber = Fiber.new do
				buffer = IO::Buffer.for("Hello World".dup)
				
				while i < repeats
					i += 1
					mutex.synchronize do
						$stderr.puts "Waiting for signal... #{i}"
						selector.io_write(Fiber.current, output, buffer, 0)
						condition.wait(mutex)
						selector.io_write(Fiber.current, output, buffer, 0)
						sleep q
					end
				end
			end
			
			selector.push(fiber)
			
			while i < repeats
				selector.select(q)
			end
			
			thread.kill
			thread.join
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		def before
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		def after
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like ThreadSafeSelector
	end
end
