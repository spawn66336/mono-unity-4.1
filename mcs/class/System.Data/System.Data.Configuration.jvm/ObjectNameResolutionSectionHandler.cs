// 
// System.Data.Configuration.ObjectNameResolutionSectionHandler.cs
//
// Authors:
//	Konstantin Triger <kostat@mainsoft.com>
//	
// (C) 2005 Mainsoft Corporation (http://www.mainsoft.com)
//

//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

using System.Collections;
using System.Configuration;
using System.Xml;

namespace System.Data.Configuration {
	class ObjectNameResolutionSectionHandler : IConfigurationSectionHandler {
		public virtual object Create (object parent, object configContext, XmlNode section) {
			if (section.Attributes != null && section.Attributes.Count != 0)
				HandlersUtil.ThrowException ("Unrecognized attribute", section);

			ObjectNameResolversCollection col = new ObjectNameResolversCollection(parent as ObjectNameResolversCollection);

			XmlNodeList resolvers = section.ChildNodes;
			foreach (XmlNode child in resolvers) {
				XmlNodeType ntype = child.NodeType;
				if (ntype == XmlNodeType.Whitespace || ntype == XmlNodeType.Comment)
					continue;

				if (ntype != XmlNodeType.Element)
					HandlersUtil.ThrowException ("Only elements allowed", child);

				string dbname = HandlersUtil.ExtractAttributeValue ("dbname", child,false,true);
				string match = HandlersUtil.ExtractAttributeValue ("match", child);
				string pri = HandlersUtil.ExtractAttributeValue ("priority", child);

				ObjectNameResolver resolver = new ObjectNameResolver(dbname, match, int.Parse(pri));
				col.Add(resolver);
			}

			col.Sort();
			
			return col;
		}
	}

	internal sealed class HandlersUtil {
		private HandlersUtil () {
		}

		static internal string ExtractAttributeValue (string attKey, XmlNode node) {
			return ExtractAttributeValue (attKey, node, false);
		}
			
		static internal string ExtractAttributeValue (string attKey, XmlNode node, bool optional) {
			return ExtractAttributeValue (attKey, node, optional, false);
		}
		
		static internal string ExtractAttributeValue (string attKey, XmlNode node, bool optional,
			bool allowEmpty) {
			if (node.Attributes == null) {
				if (optional)
					return null;

				ThrowException ("Required attribute not found: " + attKey, node);
			}

			XmlNode att = node.Attributes.RemoveNamedItem (attKey);
			if (att == null) {
				if (optional)
					return null;
				ThrowException ("Required attribute not found: " + attKey, node);
			}

			string value = att.Value;
			if (!allowEmpty && value == String.Empty) {
				string opt = optional ? "Optional" : "Required";
				ThrowException (opt + " attribute is empty: " + attKey, node);
			}

			return value;
		}

		static internal void ThrowException (string msg, XmlNode node) {
			if (node != null && node.Name != String.Empty)
				msg = msg + " (node name: " + node.Name + ") ";
			throw new ConfigurationException (msg, node);
		}
	}
}