#!/usr/local/bin/ruby

require 'shared'
$0 = "sserv.rb"
work = Queue.new

Chunks = Content # split into chunks

workers = Array.new(50)
workers.map! do
	Thread.new do
		while client = work.deq
			client.sync = true
			begin
				start = Time.new
				puts "Incoming: #{client}"

				puts "Reading..."
				l = client.gets.chomp
				if l != OutContent
					puts "Unexpected read, '#{l.dump}'"
				end
				puts "Read #{l.size} bytes, writing #{Content.size}..."

				client.print Content
				client.close
				puts "Processed in #{Time.new - start}s"
			rescue => e
				p e
			end
		end
	end
end

workers << Thread.new do
s = TCPServer.new('127.0.0.1', 9002)
while c = s.accept
	puts "Accepted: #{c}"
	work.enq c
end
end

workers.each {|w| w.join }

